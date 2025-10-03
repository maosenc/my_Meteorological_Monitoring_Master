#include "widget.h"
#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // 设置应用程序属性
    app.setApplicationName("气象数据监控系统");
    app.setApplicationVersion("1.0");

    // 针对触摸屏的设置
    app.setAttribute(Qt::AA_SynthesizeTouchForUnhandledMouseEvents, true);
    app.setAttribute(Qt::AA_SynthesizeMouseForUnhandledTouchEvents, true);

    Widget widget;
    widget.show();

    return app.exec();
}
