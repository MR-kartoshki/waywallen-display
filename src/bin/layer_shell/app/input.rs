use super::{App, PointerCtx};
use std::sync::atomic::Ordering;
use wayland_client::protocol::wl_pointer::{ButtonState, WlPointer};
use wayland_client::protocol::wl_seat::WlSeat;
use wayland_client::protocol::{wl_pointer, wl_seat};
use wayland_client::{delegate_noop, Connection, Dispatch, Proxy, QueueHandle};
use wayland_protocols::wp::cursor_shape::v1::client::wp_cursor_shape_device_v1::{
    Shape, WpCursorShapeDeviceV1,
};
use waywallen_display as sys;

delegate_noop!(App: ignore WpCursorShapeDeviceV1);

impl Dispatch<WlSeat, u32> for App {
    fn event(
        state: &mut Self,
        seat: &WlSeat,
        event: wl_seat::Event,
        data: &u32,
        _conn: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        let seat_name = *data;
        match event {
            wl_seat::Event::Capabilities { capabilities } => {
                let has_pointer = match capabilities {
                    wayland_client::WEnum::Value(c) => c.contains(wl_seat::Capability::Pointer),
                    _ => false,
                };
                let already = state.pointers.contains_key(&seat_name);
                if has_pointer && !already {
                    let pointer = seat.get_pointer(qh, seat_name);
                    state.pointers.insert(
                        seat_name,
                        PointerCtx {
                            pointer,
                            focus_output: None,
                            last_x: 0.0,
                            last_y: 0.0,
                            axis_source: 0,
                        },
                    );
                    log::info!("wl_seat name={seat_name} acquired pointer");
                } else if !has_pointer && already {
                    if let Some(ctx) = state.pointers.remove(&seat_name) {
                        ctx.pointer.release();
                    }
                    log::info!("wl_seat name={seat_name} lost pointer capability");
                }
            }
            _ => {}
        }
    }
}

impl Dispatch<WlPointer, u32> for App {
    fn event(
        state: &mut Self,
        pointer: &WlPointer,
        event: wl_pointer::Event,
        data: &u32,
        _conn: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        let seat_name = *data;
        match event {
            wl_pointer::Event::Enter {
                serial,
                surface,
                surface_x,
                surface_y,
                ..
            } => {
                let output_name = match surface.data::<u32>() {
                    Some(n) => *n,
                    None => return,
                };
                if let Some((_, manager)) = state.cursor_shape_manager.as_ref() {
                    let device = manager.get_pointer(pointer, qh, ());
                    device.set_shape(serial, Shape::Default);
                    // Destroying the device leaves its selected cursor shape in place.
                    device.destroy();
                }
                if let Some(ctx) = state.pointers.get_mut(&seat_name) {
                    ctx.focus_output = Some(output_name);
                    ctx.last_x = surface_x;
                    ctx.last_y = surface_y;
                }
            }
            wl_pointer::Event::Leave { .. } => {
                if let Some(ctx) = state.pointers.get_mut(&seat_name) {
                    ctx.focus_output = None;
                }
            }
            wl_pointer::Event::Motion {
                time: _,
                surface_x,
                surface_y,
            } => {
                let (output_name, lx, ly) = {
                    let Some(ctx) = state.pointers.get_mut(&seat_name) else {
                        return;
                    };
                    ctx.last_x = surface_x;
                    ctx.last_y = surface_y;
                    let Some(out) = ctx.focus_output else { return };
                    (out, surface_x, surface_y)
                };
                let (x, y) = logical_to_physical(state, output_name, lx, ly);
                // wl_pointer.time wraps at 32 bits and has an unspecified epoch.
                // Stamp CLOCK_MONOTONIC in the display library before enqueueing.
                send_pointer_motion(state, output_name, x, y, 0);
            }
            wl_pointer::Event::Button {
                time: _,
                button,
                state: bstate,
                ..
            } => {
                let (output_name, lx, ly) = {
                    let Some(ctx) = state.pointers.get(&seat_name) else {
                        return;
                    };
                    let Some(out) = ctx.focus_output else { return };
                    (out, ctx.last_x, ctx.last_y)
                };
                let (x, y) = logical_to_physical(state, output_name, lx, ly);
                let state_u32 = match bstate {
                    wayland_client::WEnum::Value(ButtonState::Pressed) => 1,
                    wayland_client::WEnum::Value(ButtonState::Released) => 0,
                    _ => return,
                };
                send_pointer_button(state, output_name, x, y, button, state_u32, 0);
            }
            wl_pointer::Event::Axis { axis, value, .. } => {
                let (output_name, lx, ly, src) = {
                    let Some(ctx) = state.pointers.get(&seat_name) else {
                        return;
                    };
                    let Some(out) = ctx.focus_output else { return };
                    (out, ctx.last_x, ctx.last_y, ctx.axis_source)
                };
                let (x, y) = logical_to_physical(state, output_name, lx, ly);
                let delta = (value as f32) / 10.0;
                let (dx, dy) = match axis {
                    wayland_client::WEnum::Value(wl_pointer::Axis::HorizontalScroll) => {
                        (delta, 0.0)
                    }
                    wayland_client::WEnum::Value(wl_pointer::Axis::VerticalScroll) => (0.0, delta),
                    _ => return,
                };
                send_pointer_axis(state, output_name, x, y, dx, dy, src, 0);
            }
            wl_pointer::Event::AxisSource { axis_source } => {
                if let Some(ctx) = state.pointers.get_mut(&seat_name) {
                    ctx.axis_source = match axis_source {
                        wayland_client::WEnum::Value(wl_pointer::AxisSource::Wheel) => 0,
                        wayland_client::WEnum::Value(wl_pointer::AxisSource::Finger) => 1,
                        wayland_client::WEnum::Value(wl_pointer::AxisSource::Continuous) => 2,
                        _ => 0,
                    };
                }
            }
            _ => {}
        }
    }
}

fn logical_to_physical(state: &App, output_name: u32, lx: f64, ly: f64) -> (f32, f32) {
    let Some(entry) = state.outputs.get(&output_name) else {
        return (lx as f32, ly as f32);
    };
    let Some(binding) = entry.binding.as_ref() else {
        return (lx as f32, ly as f32);
    };
    let frac = binding.fractional_scale_120.load(Ordering::Relaxed);
    let s = if frac > 0 {
        frac as f64 / 120.0
    } else {
        binding.scale.load(Ordering::Relaxed).max(1) as f64
    };
    ((lx * s) as f32, (ly * s) as f32)
}

fn send_pointer_motion(state: &App, output_name: u32, x: f32, y: f32, timestamp_us: u64) {
    let Some(entry) = state.outputs.get(&output_name) else {
        return;
    };
    let Some(binding) = entry.binding.as_ref() else {
        return;
    };
    let Some(session) = entry.session.as_ref().filter(|session| session.is_ready()) else {
        return;
    };
    let rc = unsafe {
        sys::waywallen_display_send_pointer_motion(session.display.0, x, y, timestamp_us, 0)
    };
    if rc < 0 {
        log::debug!(
            "[{}] send pointer_motion failed: {rc}",
            binding.display_name
        );
    }
}

fn send_pointer_button(
    state: &App,
    output_name: u32,
    x: f32,
    y: f32,
    button: u32,
    state_u32: u32,
    timestamp_us: u64,
) {
    let Some(binding) = state
        .outputs
        .get(&output_name)
        .and_then(|e| e.binding.as_ref())
    else {
        return;
    };
    let Some(session) = state
        .outputs
        .get(&output_name)
        .and_then(|entry| entry.session.as_ref())
        .filter(|session| session.is_ready())
    else {
        return;
    };
    let button_state = if state_u32 == 1 {
        sys::WAYWALLEN_POINTER_BUTTON_STATE_PRESSED
    } else {
        sys::WAYWALLEN_POINTER_BUTTON_STATE_RELEASED
    };
    let rc = unsafe {
        sys::waywallen_display_send_pointer_button(
            session.display.0,
            x,
            y,
            button,
            button_state,
            timestamp_us,
            0,
        )
    };
    if rc < 0 {
        log::debug!(
            "[{}] send pointer_button failed: {rc}",
            binding.display_name
        );
    }
}

fn send_pointer_axis(
    state: &App,
    output_name: u32,
    x: f32,
    y: f32,
    delta_x: f32,
    delta_y: f32,
    source: u32,
    timestamp_us: u64,
) {
    let Some(binding) = state
        .outputs
        .get(&output_name)
        .and_then(|e| e.binding.as_ref())
    else {
        return;
    };
    let Some(session) = state
        .outputs
        .get(&output_name)
        .and_then(|entry| entry.session.as_ref())
        .filter(|session| session.is_ready())
    else {
        return;
    };
    let source = match source {
        1 => sys::WAYWALLEN_POINTER_AXIS_SOURCE_FINGER,
        2 => sys::WAYWALLEN_POINTER_AXIS_SOURCE_CONTINUOUS,
        _ => sys::WAYWALLEN_POINTER_AXIS_SOURCE_WHEEL,
    };
    let rc = unsafe {
        sys::waywallen_display_send_pointer_axis(
            session.display.0,
            x,
            y,
            delta_x,
            delta_y,
            source,
            timestamp_us,
            0,
        )
    };
    if rc < 0 {
        log::debug!("[{}] send pointer_axis failed: {rc}", binding.display_name);
    }
}
