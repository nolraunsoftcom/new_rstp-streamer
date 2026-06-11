#include <QApplication>
#include <QLabel>
extern "C" {
#include <libavformat/version.h>
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QLabel label(QStringLiteral("new_viewer M1 — libavformat %1")
                     .arg(LIBAVFORMAT_VERSION_MAJOR));
    label.resize(480, 120);
    label.setAlignment(Qt::AlignCenter);
    label.show();
    return app.exec();
}
