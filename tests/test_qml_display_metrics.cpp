#include "codec.h"
#include "waywallen_display.h"
#include "ww_proto.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QMetaEnum>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QScreen>
#include <QSocketNotifier>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <cstring>
#include <memory>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

static void check(bool ok, const char* message) {
    if (! ok) qFatal("%s", message);
}

template<typename Predicate>
static void waitFor(Predicate predicate, int timeout = 2000) {
    QElapsedTimer timer;
    timer.start();
    while (! predicate()) {
        check(timer.elapsed() < timeout, "timed out waiting for display metrics");
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
}

static void pump(int milliseconds) {
    QElapsedTimer timer;
    timer.start();
    waitFor(
        [&]() {
            return timer.elapsed() >= milliseconds;
        },
        milliseconds + 1000);
}

struct Peer {
    int                                      fd;
    QSocketNotifier                          notifier;
    ww_codec_recv_state_t                    incoming {};
    QString                                  instanceId;
    waywallen_display_metrics_t              registration {};
    std::vector<waywallen_display_metrics_t> updates;

    explicit Peer(int socket): fd(socket), notifier(socket, QSocketNotifier::Read) {
        ww_codec_recv_state_init(&incoming);
        QObject::connect(&notifier, &QSocketNotifier::activated, [&]() {
            receive();
        });
    }

    ~Peer() {
        notifier.setEnabled(false);
        ww_codec_recv_state_reset(&incoming);
        ::close(fd);
    }

    template<typename Message>
    void send(uint16_t opcode, const Message& message, int (*encode)(const Message*, ww_buf_t*)) {
        ww_buf_t body;
        ww_buf_init(&body);
        check(encode(&message, &body) == WW_OK, "encode test event");
        check(ww_codec_send_event(fd, opcode, body.data, body.len, nullptr, 0) == 0,
              "send test event");
        ww_buf_free(&body);
    }

    void receive() {
        for (;;) {
            const int rc = ww_codec_recv_partial(fd, &incoming);
            if (rc == WW_CODEC_FRAME_NEED) return;
            if (rc < 0) {
                notifier.setEnabled(false);
                return;
            }
            if (incoming.op == WW_REQ_HELLO) {
                ww_evt_welcome_t welcome {};
                welcome.server_version = const_cast<char*>("metrics-test");
                send(WW_EVT_WELCOME, welcome, ww_evt_welcome_encode);
            } else if (incoming.op == WW_REQ_REGISTER_DISPLAY) {
                ww_req_register_display_t request {};
                check(ww_req_register_display_decode(incoming.body, incoming.body_len, &request) ==
                          WW_OK,
                      "decode display registration");
                instanceId   = QString::fromUtf8(request.instance_id);
                registration = request.metrics;
                ww_req_register_display_free(&request);
                ww_evt_display_accepted_t accepted {};
                accepted.display_id                            = 1;
                accepted.presentation.config.generation        = 1;
                accepted.presentation.config.pause_effect.kind = WAYWALLEN_PAUSE_EFFECT_KIND_NONE;
                accepted.presentation.config.pause_effect.blur.radius = 30;
                accepted.presentation.config.transition.kind = WAYWALLEN_TRANSITION_KIND_NONE;
                accepted.presentation.config.transition.duration_ms = 500;
                accepted.presentation.config.transition.origin_x    = 0.5f;
                accepted.presentation.config.transition.origin_y    = 0.5f;
                accepted.presentation.state.generation              = 1;
                accepted.presentation.state.config_generation       = 1;
                send(WW_EVT_DISPLAY_ACCEPTED, accepted, ww_evt_display_accepted_encode);
            } else if (incoming.op == WW_REQ_SET_DISPLAY_METRICS) {
                ww_req_set_display_metrics_t request {};
                check(ww_req_set_display_metrics_decode(
                          incoming.body, incoming.body_len, &request) == WW_OK,
                      "decode display metrics");
                updates.push_back(request.metrics);
                ww_req_set_display_metrics_free(&request);
            }
            ww_codec_recv_state_reset(&incoming);
        }
    }
};

class MockDaemon {
    int             listenFd;
    QSocketNotifier notifier;

public:
    std::vector<std::unique_ptr<Peer>> peers;

    explicit MockDaemon(const QString& path)
        : listenFd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)),
          notifier(listenFd, QSocketNotifier::Read) {
        check(listenFd >= 0, "create test listener");
        sockaddr_un address {};
        address.sun_family    = AF_UNIX;
        const QByteArray name = path.toUtf8();
        check(size_t(name.size()) < sizeof(address.sun_path), "test socket path too long");
        std::memcpy(address.sun_path, name.constData(), size_t(name.size()) + 1);
        check(::bind(listenFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
              "bind test socket");
        check(::listen(listenFd, 4) == 0, "listen on test socket");
        QObject::connect(&notifier, &QSocketNotifier::activated, [&]() {
            const int fd = ::accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
            check(fd >= 0, "accept display connection");
            peers.push_back(std::make_unique<Peer>(fd));
        });
    }

    ~MockDaemon() {
        notifier.setEnabled(false);
        ::close(listenFd);
    }
};

static void screenChanged(QQuickWindow& window, QObject& display, const char* identity) {
    check(display.setProperty("instanceId", QString::fromLatin1(identity)), "set instance id");
    window.screenChanged(window.screen());
}

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    check(argc == 2, "expected QML import path");
    QTemporaryDir temporary;
    check(temporary.isValid(), "create temporary socket directory");
    const QString socket = temporary.filePath(QStringLiteral("display.sock"));
    MockDaemon    daemon(socket);
    QQmlEngine    engine;
    engine.addImportPath(QString::fromLocal8Bit(argv[1]));
    QQmlComponent component(&engine);
    component.setData("import QtQuick\nimport QtQuick.Window\nimport " WW_TEST_QML_URI " 1.0\n"
                      "Window { id: host; visible: false; property string socketPath;"
                      "WaywallenDisplay { objectName: 'display'; socketPath: host.socketPath;"
                      "instanceId: 'screen-a'; displayWidth: 640; displayHeight: 480 } }",
                      QUrl());
    check(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> root(
        component.createWithInitialProperties({ { "socketPath", socket } }));
    check(root != nullptr, qPrintable(component.errorString()));
    auto* window  = qobject_cast<QQuickWindow*>(root.get());
    auto* display = root->findChild<QObject*>(QStringLiteral("display"));
    check(window && display, "create test display");
    check(QMetaObject::invokeMethod(display, "requestReconnect"), "request initial connection");
    const QMetaEnum states =
        display->metaObject()->enumerator(display->metaObject()->indexOfEnumerator("ConnState"));
    const int connected = states.keyToValue("Connected");
    waitFor([&]() {
        return display->property("connState").toInt() == connected;
    });
    check(daemon.peers.size() == 1 && daemon.peers[0]->instanceId == "screen-a",
          "register screen A");

    display->setProperty("displayWidth", 800);
    waitFor([&]() {
        return daemon.peers[0]->updates.size() == 1;
    });
    check(daemon.peers[0]->updates.back().width == 800, "ordinary resize must still be sent");
    daemon.peers[0]->updates.clear();

    screenChanged(*window, *display, "screen-b");
    display->setProperty("displayWidth", 1920);
    display->setProperty("displayHeight", 1080);
    window->screen()->refreshRateChanged(window->screen()->refreshRate());
    pump(250);
    check(daemon.peers[0]->updates.empty(), "screen B metrics leaked to screen A during debounce");
    screenChanged(*window, *display, "screen-a");
    waitFor([&]() {
        return daemon.peers[0]->updates.size() == 1;
    });
    check(daemon.peers[0]->updates.back().width == 1920 &&
              daemon.peers[0]->updates.back().height == 1080,
          "returning to screen A must send the deferred current size");
    pump(350);
    check(daemon.peers.size() == 1, "transient identity change must not reconnect");
    daemon.peers[0]->updates.clear();

    screenChanged(*window, *display, "screen-b");
    display->setProperty("displayWidth", 3840);
    display->setProperty("displayHeight", 2160);
    pump(250);
    check(daemon.peers[0]->updates.empty(), "screen B resize must not reach the old session");
    waitFor([&]() {
        return daemon.peers.size() == 2 && daemon.peers[1]->instanceId == "screen-b" &&
               display->property("connState").toInt() == connected;
    });
    check(daemon.peers[1]->registration.width == 3840 &&
              daemon.peers[1]->registration.height == 2160,
          "screen B handshake must carry its current size");
    check(daemon.peers[0]->updates.empty(), "old identity received metrics before reconnect");
    display->setProperty("displayWidth", 2560);
    waitFor([&]() {
        return daemon.peers[1]->updates.size() == 1;
    });
    check(daemon.peers[1]->updates.back().width == 2560,
          "new identity must accept ordinary resize updates");
    std::puts("test_qml_display_metrics: OK");
}
