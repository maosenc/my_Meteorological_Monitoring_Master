#include "widget.h"
#include <QApplication>
#include <QScreen>
#include <QGridLayout>
#include <qdebug.h>
#include <random>
#include <ctime>
#include <unistd.h>  // for getpid()
#include <cerrno>    // for errno
#include <cstring>   // for strerror()
#include <QTimeZone>


Widget::Widget(QWidget *parent)
    : QWidget(parent)
    , m_sharedData(nullptr)
    , m_shmId(-1)
    , m_sharedMemoryValid(false)
    , m_lastUpdateCounter(0)
    , m_currentStationIndex(0)
    , m_useRealData(false)
{
    setupUI(); //设置主窗口的布局、大小和外观。

    // 设置窗口属性适配800*480触摸屏
    this->setFixedSize(800, 480);
    this->setWindowTitle(QString::fromUtf8("气象数据监控系统"));

    // 居中显示
    QScreen *screen = QApplication::primaryScreen();
    QRect screenGeometry = screen->geometry();
    int x = (screenGeometry.width() - this->width()) / 2;
    int y = (screenGeometry.height() - this->height()) / 2;
    this->move(x, y);

    // 设置主窗口样式
    this->setStyleSheet("Widget { background-color: #ffffff; }");

    // 初始化时间显示
    updateTimeDisplay();

    // 初始化共享内存
    qDebug() << "Attempt to initialize shared memory";

    if (!initSharedMemory()){
        qDebug() << "Shared memory initialization failed";
    }
    else  {
        qDebug() << "Shared memory initialization successful";
    }

    // 创建并启动定时器
    m_timeUpdateTimer = new QTimer(this);
    connect(m_timeUpdateTimer, &QTimer::timeout, this, &Widget::updateTimeDisplay);
    m_timeUpdateTimer->start(100); // 更新一次时间

    // 创建系统信息更新定时器
    m_updateSystemInfoTimer = new QTimer(this);
    connect(m_updateSystemInfoTimer, &QTimer::timeout, this, &Widget::updateSystemInfoDisplay);
    m_updateSystemInfoTimer->start(100); // 每秒更新一次系统信息显示


    // 创建数据检查定时器
    m_dataCheckTimer = new QTimer(this);
    connect(m_dataCheckTimer, &QTimer::timeout, this, &Widget::checkSharedMemoryUpdate);
    m_dataCheckTimer->start(2000); // 检查一次数据更新

    qDebug() << "Qt application started, PID:" << getpid();
}



Widget::~Widget()
{
    cleanupSharedMemory();
}

bool Widget::initSharedMemory()
{
    // 获取共享内存
    m_shmId = shmget(SHARED_MEMORY_KEY, 0, 0);
    if (m_shmId == -1) {
    //    qDebug() << "Shared memory does not exist, please start receiver program first";
    //    qDebug() << "shmget error:" << strerror(errno);
        return false;
    }

    // 连接到共享内存
    m_sharedData = (struct shared_weather_data *)shmat(m_shmId, NULL, 0);
    if (m_sharedData == (void *)-1) {
        qDebug() << "Failed to attach shared memory:" << strerror(errno);
        m_sharedData = nullptr;
        return false;
    }

    // 验证魔数
    if (m_sharedData->magic != SHARED_MEMORY_MAGIC) {
        qDebug() << "Shared memory magic number mismatch. Expected:" << QString::number(SHARED_MEMORY_MAGIC, 16)
                 << "Got:" << QString::number(m_sharedData->magic, 16);
        shmdt(m_sharedData);
        m_sharedData = nullptr;
        return false;
    }

    // 设置读进程PID
    m_sharedData->reader_pid = getpid();
    m_sharedMemoryValid = true;

    // 设置为0以确保第一次检查时会触发更新
    m_lastUpdateCounter = 0;

    qDebug() << "Successfully connected to shared memory, writer PID:" << m_sharedData->writer_pid;
    qDebug() << "Current update counter:" << m_sharedData->update_counter;
    qDebug() << "Connection status:" << m_sharedData->connection_status;

    // 立即更新状态显示
    updateConnectionStatus();
    updateSystemInfoDisplay();
    updateDataFromSharedMemory();

    return true;
}

void Widget::cleanupSharedMemory()
{
    if (m_sharedData != nullptr) {
        m_sharedData->reader_pid = 0; // 清除读进程PID
        if (shmdt(m_sharedData) == -1) {
            qDebug() << "Failed to detach shared memory:" << strerror(errno);
        }
        m_sharedData = nullptr;
    }
    m_sharedMemoryValid = false;
}

void Widget::updateSystemInfoDisplay()
{
    if (!m_sharedMemoryValid || m_sharedData == nullptr) {
        QString info;
            info += QString("数据源: 未连接\n");
            info += QString("连接状态: 共享内存未连接\n");
            info += QString("请先启动接收程序\n");
            info += QString("当前时间: %1\n").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss"));
            info += QString("系统状态: 等待连接中...\n");

        m_systemInfoText->setText(info);
        return;
    }

    QString info;
    info += QString("Data source: %1:%2\n")
            .arg(m_sharedData->server_ip)
            .arg(m_sharedData->server_port);

    QString statusText;
    switch (m_sharedData->connection_status) {
        case CONNECTION_CONNECTED:
            statusText = "Connected";
            break;
        case CONNECTION_CONNECTING:
            statusText = "Connecting";
            break;
        default:
            statusText = "Disconnected";
            break;
    }
    info += QString("Connection status: %1\n").arg(statusText);

    info += QString("Writer PID: %1\n").arg(m_sharedData->writer_pid);
    info += QString("Update counter: %1\n").arg(m_sharedData->update_counter);
    info += QString("Total received: %1 frames\n").arg(m_sharedData->total_received);
    info += QString("Error frames: %1\n").arg(m_sharedData->total_errors);

    // 显示各类型数据统计
    info += QString("BME280: %1, Light: %2\n").arg(m_sharedData->bme280_count).arg(m_sharedData->lightrain_count);
    info += QString("GPS: %1, Status: %2\n").arg(m_sharedData->gps_count).arg(m_sharedData->system_status_count);

    if (m_sharedData->last_update_time > 0) {
        QDateTime lastUpdate = QDateTime::fromSecsSinceEpoch(m_sharedData->last_update_time);
        info += QString("Last update: %1\n").arg(lastUpdate.toString("hh:mm:ss"));
    }

    if (strlen(m_sharedData->last_error) > 0) {
        info += QString("Last error: %1").arg(m_sharedData->last_error);
    }

    m_systemInfoText->setText(info);
}

void Widget::checkSharedMemoryUpdate()
{
    if (!m_sharedMemoryValid || m_sharedData == nullptr) {
        if (!initSharedMemory()) {
            return;
        }
    }

    if (m_sharedMemoryValid && m_sharedData != nullptr){
        // 检查数据是否有更新
        if (m_sharedData->update_counter != m_lastUpdateCounter || m_lastUpdateCounter == 0) {
            //qDebug() << "Data updated, counter changed from" << m_lastUpdateCounter
            //         << "to" << m_sharedData->update_counter;
            updateDataFromSharedMemory();
            m_lastUpdateCounter = m_sharedData->update_counter;
        }
    }

    // 更新连接状态
    updateConnectionStatus();
}

QString Widget::getStationNameFromNodeId(int nodeId){
    //根据node_id返回站点名称
    switch(nodeId){
        case 1: return QString::fromUtf8("主站点");
        case 2: return QString::fromUtf8("上海站");
        case 3: return QString::fromUtf8("广州站");
        case 4: return QString::fromUtf8("成都站");
        case 5: return QString::fromUtf8("西安站");
        case 6: return QString::fromUtf8("武汉站");
        default: return QString::fromUtf8("未知站点");
    }
}

void Widget::updateDataFromSharedMemory()
{
    if (!m_sharedMemoryValid || m_sharedData == nullptr) {
        return;
    }

    //qDebug() << "Updating data from shared memory, latest data type:" << m_sharedData->latest_data.data_type;

    // 根据最新数据类型更新显示
    switch (m_sharedData->latest_data.data_type) {
        case SENSOR_BME280:
            if (m_sharedData->latest_bme280.valid) {
                // 获取node_id并查找对应的站点名称
                int nodeId = m_sharedData->latest_data.data.bme280.node_id;
                QString stationName = getStationNameFromNodeId(nodeId);
                if (stationName.isEmpty()) {
                        stationName = QString::fromUtf8("未知站点");
                    }
                if(!m_stationWidgets.isEmpty()){
                    m_stationWidgets[nodeId]->setStationName(stationName);
                }
                updateStationWithBME280Data(m_sharedData->latest_bme280, nodeId);
                // 更新告警状态
                updateAlertWithLatestData(nodeId);
            //    qDebug() << "Updated BME280 data: T=" << m_sharedData->latest_bme280.temperature
            //             << "°C, H=" << m_sharedData->latest_bme280.humidity
            //             << "%, P=" << m_sharedData->latest_bme280.pressure << "hPa";
            }
            break;

        case SENSOR_LIGHTRAIN:
            if (m_sharedData->latest_lightrain.valid) {
                // 获取node_id并查找对应的站点名称
                int nodeId = m_sharedData->latest_data.data.bme280.node_id;
                QString stationName = getStationNameFromNodeId(nodeId);
                if (stationName.isEmpty()) {
                        stationName = QString::fromUtf8("未知站点");
                    }
                if(!m_stationWidgets.isEmpty()){
                    m_stationWidgets[nodeId]->setStationName(stationName);
                }
                updateStationWithLightRainData(m_sharedData->latest_lightrain, nodeId);
                // 更新告警状态
                updateAlertWithLatestData(nodeId);
            //    qDebug() << "Updated LightRain data: Lux=" << m_sharedData->latest_lightrain.light_intensity
            //             << "lx, Rain=" << m_sharedData->latest_lightrain.rainfall << "%";
            }
            break;

        case SENSOR_GPS:
            if (m_sharedData->latest_gps.valid) {
                // 获取node_id并查找对应的站点名称
                int nodeId = m_sharedData->latest_data.data.bme280.node_id;
                QString stationName = getStationNameFromNodeId(nodeId);
                if (stationName.isEmpty()) {
                        stationName = QString::fromUtf8("未知站点");
                    }
                if(!m_stationWidgets.isEmpty()){
                    m_stationWidgets[nodeId]->setStationName(stationName);
                }
                updateStationWithGPSData(m_sharedData->latest_gps, nodeId);
                // 更新告警状态
                updateAlertWithLatestData(nodeId);
            //    qDebug() << "Updated GPS data: Lat=" << m_sharedData->latest_gps.latitude
            //             << ", Lon=" << m_sharedData->latest_gps.longitude;
            }
            break;

        case SENSOR_SYSTEM_STATUS:
            if (m_sharedData->latest_system_status.valid) {
            //   qDebug() << "Updated System Status: Node=" << m_sharedData->latest_system_status.node_id
            //             << ", Uptime=" << m_sharedData->latest_system_status.uptime_seconds << "s";
            }
            break;

        default:
            //qDebug() << "Unknown data type:" << m_sharedData->latest_data.data_type;
            break;
    }



//    // 为其他站点生成随机数据
//    simulateRandomDataForOtherStations(0);

    m_useRealData = true;
}

void Widget::updateStationWithBME280Data(const struct bme280_data &data, int stationIndex)
{
    if (stationIndex >= 0 && stationIndex < m_stationWidgets.size()) {
        m_stationWidgets[stationIndex]->updateBME280Data(data);
    }
}

void Widget::updateStationWithLightRainData(const struct lightrain_data &data, int stationIndex)
{
    if (stationIndex >= 0 && stationIndex < m_stationWidgets.size()) {
        m_stationWidgets[stationIndex]->updateLightRainData(data);
    }
}

void Widget::updateStationWithGPSData(const struct gps_data &data, int stationIndex)
{
    if (stationIndex >= 0 && stationIndex < m_stationWidgets.size()) {
        m_stationWidgets[stationIndex]->updateGPSData(data);
    }
}

void Widget::updateAlertWithLatestData(int stationIndex)
{
    if (stationIndex >= 0 && stationIndex < m_alertWidgets.size()) {
        QString alertType, alertMessage, status = QString::fromUtf8("正常");

        // 检查BME280数据告警
        if (m_sharedData->latest_bme280.valid) {
            if (m_sharedData->latest_bme280.temperature > 35.0f) {
                alertType = QString::fromUtf8("温度");
                alertMessage = QString::fromUtf8("极高温警报：%1°C").arg(m_sharedData->latest_bme280.temperature, 0, 'f', 1);
                status = QString::fromUtf8("警报");
            } else if (m_sharedData->latest_bme280.temperature > 30.0f) {
                alertType = QString::fromUtf8("温度");
                alertMessage = QString::fromUtf8("高温告警：%1°C").arg(m_sharedData->latest_bme280.temperature, 0, 'f', 1);
                status = QString::fromUtf8("告警");
            } else if (m_sharedData->latest_bme280.humidity > 90) {
                alertType = QString::fromUtf8("湿度");
                alertMessage = QString::fromUtf8("高湿度警报：%1%").arg(m_sharedData->latest_bme280.humidity, 0, 'f', 1);
                status = QString::fromUtf8("警报");
            } else if (m_sharedData->latest_bme280.humidity > 80) {
                alertType = QString::fromUtf8("湿度");
                alertMessage = QString::fromUtf8("湿度告警：%1%").arg(m_sharedData->latest_bme280.humidity, 0, 'f', 1);
                status = QString::fromUtf8("告警");
            }
        }

        // 检查光强雨量数据告警
        if (status == QString::fromUtf8("正常") && m_sharedData->latest_lightrain.valid) {
            if (m_sharedData->latest_lightrain.rainfall > 80) {
                alertType = QString::fromUtf8("降雨");
                alertMessage = QString::fromUtf8("强降雨警报：%1%").arg(m_sharedData->latest_lightrain.rainfall);
                status = QString::fromUtf8("警报");
            } else if (m_sharedData->latest_lightrain.rainfall > 50) {
                alertType = QString::fromUtf8("降雨");
                alertMessage = QString::fromUtf8("降雨告警：%1%").arg(m_sharedData->latest_lightrain.rainfall);
                status = QString::fromUtf8("告警");
            }
        }

        m_alertWidgets[stationIndex]->updateAlertData(alertType, alertMessage, status);
    }
}

void Widget::updateConnectionStatus()
{
    if (!m_sharedMemoryValid || m_sharedData == nullptr) {
        m_connectionStatusLabel->setText(QString::fromUtf8("Shared memory not connected"));
        return;
    }

    QString statusText;
    switch (m_sharedData->connection_status) {
        case CONNECTION_CONNECTED:
            statusText = QString::fromUtf8("Connected (%1:%2)")
                        .arg(m_sharedData->server_ip)
                        .arg(m_sharedData->server_port);
            break;
        case CONNECTION_CONNECTING:
            statusText = QString::fromUtf8("Connecting...");
            break;
        case CONNECTION_DISCONNECTED:
        default:
            statusText = QString::fromUtf8("Disconnected");
            break;
    }
    m_connectionStatusLabel->setText(statusText);
}

void Widget::setupUI()
{
    // 创建主垂直布局
    m_mainLayout = new QVBoxLayout(this);
    m_mainLayout->setContentsMargins(10, 10, 10, 10);
    m_mainLayout->setSpacing(10);

    // 创建上下两个主要区域
    m_topWidget = new QWidget();
    m_bottomWidget = new QWidget();

    // 设置上下区域的样式
    m_topWidget->setStyleSheet("QWidget { "
                               "background-color: #e6f3ff; "
                               "border: none; "
                               "border-radius: 10px; }");

    m_bottomWidget->setStyleSheet("QWidget { "
                                  "background-color: #ffffff; "
                                  "border: none; "
                                  "border-radius: 10px; }");

    // 设置上下区域的固定高度
    m_topWidget->setFixedHeight(70);
    m_bottomWidget->setFixedHeight(410);

    // 将上下区域添加到主布局
    m_mainLayout->addWidget(m_topWidget);
    m_mainLayout->addWidget(m_bottomWidget);

    // 设置上下区域的内部布局
    setupTopWidget();
    setupBottomWidget();
}

void Widget::setupTopWidget()
{
    // 创建上部分的水平布局
    m_topLayout = new QHBoxLayout(m_topWidget);
    m_topLayout->setContentsMargins(15, 10, 15, 10);
    m_topLayout->setSpacing(10);

    // 左上角标题标签
    m_titleLabel = new QLabel(QString::fromUtf8("多站气象数据监控系统"));
    m_titleLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_titleLabel->setStyleSheet("QLabel { "
                               "font-size: 20px; "
                               "font-weight: bold; "
                               "color: #1e3a8a; "
                               "background-color: transparent; "
                               "border: none; }");

    // 中间时间显示
    m_timeLabel = new QLabel();
    m_timeLabel->setAlignment(Qt::AlignCenter);
    m_timeLabel->setStyleSheet("QLabel { "
                              "font-size: 16px; "
                              "font-weight: bold; "
                              "color: #1e3a8a; "
                              "background-color: transparent; "
                              "border: none; }");

    // 连接状态标签
    m_connectionStatusLabel = new QLabel(QString::fromUtf8("检查连接中..."));
    m_connectionStatusLabel->setAlignment(Qt::AlignCenter);
    m_connectionStatusLabel->setStyleSheet("QLabel { "
                                          "font-size: 11px; "
                                          "color: #757575; "
                                          "background-color: transparent; "
                                          "border: none; }");

    // 系统信息按钮
    m_infoButton = new QPushButton(QString::fromUtf8("系统信息"));
    m_infoButton->setFixedSize(80, 35);
    m_infoButton->setStyleSheet("QPushButton { "
                               "font-size: 12px; "
                               "color: white; "
                               "background-color: #4caf50; "
                               "border: none; "
                               "border-radius: 6px; "
                               "padding: 5px 10px; } "
                               "QPushButton:hover { "
                               "background-color: #45a049; } "
                               "QPushButton:pressed { "
                               "background-color: #3d8b40; }");



    // 连接按钮信号
    connect(m_infoButton, &QPushButton::clicked, this, &Widget::showSystemInfo);


    // 添加到水平布局
    m_topLayout->addWidget(m_titleLabel);
    m_topLayout->addStretch();
    m_topLayout->addWidget(m_timeLabel);
    m_topLayout->addStretch();
    m_topLayout->addWidget(m_connectionStatusLabel);
    m_topLayout->addWidget(m_infoButton);

}

void Widget::setupBottomWidget()
{
    // 创建下部分的水平布局
    m_bottomLayout = new QHBoxLayout(m_bottomWidget);
    m_bottomLayout->setContentsMargins(5, 5, 5, 5);
    m_bottomLayout->setSpacing(5);

    // 创建左右两个子区域
    m_leftWidget = new QWidget();
    m_rightWidget = new QWidget();

    // 设置固定宽度
    m_leftWidget->setFixedWidth(570);
    m_rightWidget->setFixedWidth(215);

    // 设置子区域样式
    m_leftWidget->setStyleSheet("QWidget { "
                               "background-color: #ffffff; "
                               "border: none; "
                               "border-radius: 5px; }");

    m_rightWidget->setStyleSheet("QWidget { "
                                "background-color: #f8f9fa; "
                                "border: none; "
                                "border-radius: 5px; }");

    // 设置左侧网格布局
    setupLeftWidget();
    // 设置右侧垂直布局
    setuprightWidget();

    // 将左右区域添加到水平布局
    m_bottomLayout->addWidget(m_leftWidget);
    m_bottomLayout->addWidget(m_rightWidget);
}

void Widget::setupLeftWidget()
{
    // 创建网格布局
    m_gridLayout = new QGridLayout(m_leftWidget);
    m_gridLayout->setContentsMargins(8, 8, 8, 8);
    m_gridLayout->setSpacing(6);

    // 创建气象站widget - 2行3列布局
    QStringList stationNames = {
        QString::fromUtf8("主站点"), QString::fromUtf8("上海站"), QString::fromUtf8("广州站"),
        QString::fromUtf8("成都站"), QString::fromUtf8("西安站"), QString::fromUtf8("武汉站")
    };

    int row = 0, col = 0;
    for (const QString& name : stationNames) {
        WeatherStationWidget* stationWidget = new WeatherStationWidget(name, this);
        m_stationWidgets.append(stationWidget);

        // 添加到网格布局
        m_gridLayout->addWidget(stationWidget, row, col);

        // 更新行列索引
        col++;
        if (col >= 3) {
            col = 0;
            row++;
        }
    }

    // 设置行列拉伸因子，使所有widget均匀分布
    for (int i = 0; i < 3; ++i) {
        m_gridLayout->setColumnStretch(i, 1);
    }
    for (int i = 0; i < 2; ++i) {
        m_gridLayout->setRowStretch(i, 1);
    }

    // 主站点高亮显示
    if (!m_stationWidgets.isEmpty()) {
        m_stationWidgets[0]->setHighlighted(true);
    }

    // 初始化一些示例数据
    if (m_stationWidgets.size() >= 6) {
        m_stationWidgets[0]->updateData(23.5, 65.2, 1013.2, 25000, 12.5, 0.0);
        m_stationWidgets[1]->updateData(26.8, 78.1, 1015.6, 30000, 15.2, 2.3);
        m_stationWidgets[2]->updateData(29.2, 82.5, 1012.8, 28000, 18.7, 0.5);
        m_stationWidgets[3]->updateData(22.1, 58.9, 1016.3, 32000, 10.8, 0.0);
        m_stationWidgets[4]->updateData(18.7, 45.2, 1018.7, 35000, 8.3, 0.0);
        m_stationWidgets[5]->updateData(25.3, 72.1, 1014.5, 26000, 14.9, 1.2);
    }
}

void Widget::setuprightWidget()
{
    // 创建右侧垂直布局
    m_rightLayout = new QVBoxLayout(m_rightWidget);
    m_rightLayout->setContentsMargins(5, 5, 5, 5);
    m_rightLayout->setSpacing(5);

    m_upperWidget = new QWidget();   // 上边区域 - 系统信息
    m_middleWidget = new QWidget();  // 中间区域 - 告警信息
    m_lowerWidget = new QWidget();   // 下区域 - 图例

    // 设置各区域的高度
    m_upperWidget->setFixedHeight(120);
    m_lowerWidget->setFixedHeight(50);

    // 设置各区域的样式
    m_upperWidget->setStyleSheet("QWidget { "
                                 "background-color: #f8f9fa; "
                                 "border: none; "
                                 "border-radius: 5px; }");

    m_middleWidget->setStyleSheet("QWidget { "
                                  "background-color: #ffffff; "
                                  "border: none; "
                                  "border-radius: 5px; }");

    m_lowerWidget->setStyleSheet("QWidget { "
                                "background-color: #f8f9fa; "
                                "border: none; "
                                "border-radius: 5px; }");

    // 设置各区域的内容
    setupSystemInfoWidget();
    setupAlertScrollArea();
    setupLegendWidget();

    // 将上中下区域添加到垂直布局
    m_rightLayout->addWidget(m_upperWidget);
    m_rightLayout->addWidget(m_middleWidget);
    m_rightLayout->addWidget(m_lowerWidget);

    // 设置拉伸因子
    m_rightLayout->setStretch(0, 0);  // 上部区域固定高度
    m_rightLayout->setStretch(1, 1);  // 中间区域可拉伸
    m_rightLayout->setStretch(2, 0);  // 下部区域固定高度
}

void Widget::setupSystemInfoWidget()
{
    // 创建系统信息区域的布局
    QVBoxLayout *infoLayout = new QVBoxLayout(m_upperWidget);
    infoLayout->setContentsMargins(8, 8, 8, 8);
    infoLayout->setSpacing(5);

    // 标题
    QLabel *titleLabel = new QLabel(QString::fromUtf8("系统状态"));
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setStyleSheet("QLabel { "
                             "font-size: 12px; "
                             "font-weight: bold; "
                             "color: #1e3a8a; "
                             "background-color: transparent; }");

    // 系统信息文本框
    m_systemInfoText = new QTextEdit();
    m_systemInfoText->setReadOnly(true);
    m_systemInfoText->setMinimumHeight(80);
    m_systemInfoText->setStyleSheet("QTextEdit { "
                                   "font-size: 9px; "
                                   "color: #666666; "
                                   "background-color: #ffffff; "
                                   "border: 1px solid #ddd; "
                                   "border-radius: 3px; "
                                   "padding: 3px; }");

    // 初始化系统信息
    m_systemInfoText->setText("Waiting for shared memory data...");

    infoLayout->addWidget(titleLabel);
    infoLayout->addWidget(m_systemInfoText);
}

void Widget::setupAlertScrollArea()
{
    // 创建中间区域的布局
    QVBoxLayout *middleLayout = new QVBoxLayout(m_middleWidget);
    middleLayout->setContentsMargins(0, 0, 0, 0);
    middleLayout->setSpacing(0);

    // 创建Scroll Area
    m_scrollArea = new QScrollArea(m_middleWidget);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scrollArea->setFrameShape(QFrame::NoFrame);

    // 美化滚动条样式
    m_scrollArea->setStyleSheet(
        "QScrollArea { "
        "    background-color: transparent; "
        "    border: none; "
        "} "
        "QScrollBar:vertical { "
        "    background-color: #f5f5f5; "
        "    width: 12px; "
        "    border: none; "
        "    border-radius: 6px; "
        "} "
        "QScrollBar::handle:vertical { "
        "    background-color: #c0c0c0; "
        "    border-radius: 6px; "
        "    min-height: 30px; "
        "} "
        "QScrollBar::handle:vertical:hover { "
        "    background-color: #a0a0a0; "
        "} "
        "QScrollBar::handle:vertical:pressed { "
        "    background-color: #808080; "
        "} "
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { "
        "    border: none; "
        "    background: none; "
        "} "
    );

    // 创建Scroll Area的内容widget
    m_scrollContentWidget = new QWidget();
    m_scrollContentWidget->setFixedWidth(190);

    // 创建内容widget的垂直布局
    m_scrollLayout = new QVBoxLayout(m_scrollContentWidget);
    m_scrollLayout->setContentsMargins(5, 5, 5, 5);
    m_scrollLayout->setSpacing(3);

    // 创建6个告警信息widget
    QStringList stationNames = {
        QString::fromUtf8("主站点"), QString::fromUtf8("上海站"), QString::fromUtf8("广州站"),
        QString::fromUtf8("成都站"), QString::fromUtf8("西安站"), QString::fromUtf8("武汉站")
    };

    for (const QString& name : stationNames) {
        AlertStationWidget* alertWidget = new AlertStationWidget(name, this);
        m_alertWidgets.append(alertWidget);
        m_scrollLayout->addWidget(alertWidget);
    }

    // 添加弹簧，使内容从顶部开始
    m_scrollLayout->addStretch();

    // 设置Scroll Area的widget
    m_scrollArea->setWidget(m_scrollContentWidget);
    middleLayout->addWidget(m_scrollArea);

    // 初始化告警数据
    if (m_alertWidgets.size() >= 6) {
        m_alertWidgets[0]->updateAlertData("", QString::fromUtf8("等待数据..."), QString::fromUtf8("离线"));
        for (int i = 1; i < m_alertWidgets.size(); ++i) {
            m_alertWidgets[i]->updateAlertData("", "", QString::fromUtf8("正常"));
        }
    }
}

void Widget::setupLegendWidget()
{
    // 创建图例区域的水平布局
    QHBoxLayout *legendLayout = new QHBoxLayout(m_lowerWidget);
    legendLayout->setContentsMargins(8, 10, 8, 10);
    legendLayout->setSpacing(8);

    // 创建四种状态的图例
    struct LegendItem {
        QString color;
        QString text;
    };

    QVector<LegendItem> legends = {
        {"#4caf50", QString::fromUtf8("正常")},
        {"#ff9800", QString::fromUtf8("告警")},
        {"#f44336", QString::fromUtf8("警报")},
        {"#757575", QString::fromUtf8("离线")}
    };

    for (const auto& legend : legends) {
        // 创建颜色指示器
        QLabel *colorIndicator = new QLabel(QString::fromUtf8("●"));
        colorIndicator->setFixedSize(20, 20);
        colorIndicator->setAlignment(Qt::AlignCenter);
        colorIndicator->setStyleSheet(QString("QLabel { "
                                            "color: %1; "
                                            "font-size: 14px; "
                                            "font-weight: bold; "
                                            "border: none; "
                                            "background-color: transparent; }").arg(legend.color));

        // 创建文字说明
        QLabel *textLabel = new QLabel(legend.text);
        textLabel->setStyleSheet("QLabel { "
                                "font-size: 8px; "
                                "color: #666666; "
                                "border: none; "
                                "background-color: transparent; }");

        legendLayout->addWidget(colorIndicator);
        legendLayout->addWidget(textLabel);
    }

    legendLayout->addStretch();
}

void Widget::updateTimeDisplay()
{
    // 创建东八区时区对象
    QTimeZone beijingTimeZone(8 * 3600); // 东八区 UTC+8
    // 获取当前本地时间
    QDateTime currentTime = QDateTime::currentDateTime();
    QDateTime beijingTime = currentTime.toTimeZone(beijingTimeZone);
    //qDebug() << "Beijing Time:" << beijingTime;
    // 显示北京时间
    m_timeLabel->setText(beijingTime.toString("yyyy-MM-dd hh:mm:ss"));
}


void Widget::showSystemInfo()
{
    if (!m_sharedMemoryValid || m_sharedData == nullptr) {
        m_systemInfoText->setText("Shared memory not connected\nPlease start receiver program");
        return;
    }

    updateSystemInfoDisplay();
}

void Widget::simulateRandomDataForOtherStations(int excludeIndex)
{
    // 为其他站点生成随机数据
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<int> percentDist(0, 99);
    std::uniform_int_distribution<int> tempDist(15, 40);
    std::uniform_int_distribution<int> humidityDist(30, 90);
    std::uniform_int_distribution<int> pressureDist(995, 1030);
    std::uniform_int_distribution<int> lightDist(10000, 50000);
    std::uniform_int_distribution<int> vaporDist(5, 25);
    std::uniform_int_distribution<int> rainDist(0, 50);

    for (int i = 0; i < m_stationWidgets.size(); ++i) {
        if (i == excludeIndex) continue; // 跳过真实数据站点

        // 生成随机数据
        double temp = tempDist(gen);
        double humidity = humidityDist(gen);
        double pressure = pressureDist(gen);
        double lightIntensity = lightDist(gen);
        double waterVapor = vaporDist(gen) / 10.0;
        double rainfall = rainDist(gen) / 10.0;

        // 更新显示
        m_stationWidgets[i]->updateData(temp, humidity, pressure, lightIntensity, waterVapor, rainfall);

        // 模拟告警状态
        int rand = percentDist(gen);
        if (rand < 15) {
            m_alertWidgets[i]->updateAlertData("", "", QString::fromUtf8("离线"));
        } else if (rand < 25) {
            QString alertType = QString::fromUtf8("温度");
            QString alertMessage = QString::fromUtf8("极高温警报：%1°C").arg(temp, 0, 'f', 1);
            m_alertWidgets[i]->updateAlertData(alertType, alertMessage, QString::fromUtf8("警报"));
        } else if (rand < 45) {
            QString alertType = QString::fromUtf8("温度");
            QString alertMessage = QString::fromUtf8("高温告警：%1°C").arg(temp, 0, 'f', 1);
            m_alertWidgets[i]->updateAlertData(alertType, alertMessage, QString::fromUtf8("告警"));
        } else {
            m_alertWidgets[i]->updateAlertData("", "", QString::fromUtf8("正常"));
        }
    }
}

void Widget::updateWeatherData()
{
    //qDebug() << "Manual data refresh -" << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");

    // 模拟所有站点的随机数据
    simulateRandomDataForOtherStations(-1); // -1表示为所有站点生成数据
}

// WeatherStationWidget 实现
WeatherStationWidget::WeatherStationWidget(const QString& stationName, QWidget *parent)
    : QFrame(parent), m_stationName(stationName), m_isHighlighted(false)
{
    setupUI();

    // 初始化当前数据
    m_currentData.temperature = 0.0;
    m_currentData.humidity = 0.0;
    m_currentData.pressure = 1013.0;
    m_currentData.lightIntensity = 25000.0;
    m_currentData.waterVapor = 12.5;
    m_currentData.rainfall = 0.0;
}
void WeatherStationWidget::setStationName(const QString &stationName){
    m_stationName = stationName;
    if(m_nameLabel){
        m_nameLabel->setText(stationName);
    }
}

void WeatherStationWidget::setupUI()
{
    // 设置框架样式
    this->setFrameStyle(QFrame::NoFrame);
    setHighlighted(m_isHighlighted);

    // 创建垂直布局
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(1);

    // 站点名称
    m_nameLabel = new QLabel(m_stationName);
    m_nameLabel->setAlignment(Qt::AlignCenter);
    m_nameLabel->setStyleSheet("QLabel { "
                              "font-size: 12px; "
                              "font-weight: bold; "
                              "color: #1565c0; "
                              "background-color: transparent; "
                              "border: none; "
                              "padding: 3px; }");

    // 添加分隔线
    QFrame *separatorLine = new QFrame();
    separatorLine->setFrameShape(QFrame::HLine);
    separatorLine->setStyleSheet("QFrame { "
                               "color: #90caf9; "
                               "background-color: #90caf9; "
                               "border: none; "
                               "height: 1px; }");

    // 各项数据标签
    m_tempLabel = new QLabel(QString::fromUtf8("温度: --°C"));
    m_tempLabel->setStyleSheet("QLabel { font-size: 10px; color: #d32f2f; background-color: transparent; border: none; }");

    m_humidityLabel = new QLabel(QString::fromUtf8("湿度: --%"));
    m_humidityLabel->setStyleSheet("QLabel { font-size: 10px; color: #1976d2; background-color: transparent; border: none; }");

    m_pressureLabel = new QLabel(QString::fromUtf8("气压: --hPa"));
    m_pressureLabel->setStyleSheet("QLabel { font-size: 10px; color: #388e3c; background-color: transparent; border: none; }");

    m_lightLabel = new QLabel(QString::fromUtf8("光强: --lux"));
    m_lightLabel->setStyleSheet("QLabel { font-size: 10px; color: #ff9800; background-color: transparent; border: none; }");

    m_waterVaporLabel = new QLabel(QString::fromUtf8("水汽: --g/m³"));
    m_waterVaporLabel->setStyleSheet("QLabel { font-size: 10px; color: #00bcd4; background-color: transparent; border: none; }");

    m_rainfallLabel = new QLabel(QString::fromUtf8("雨量: --mm"));
    m_rainfallLabel->setStyleSheet("QLabel { font-size: 10px; color: #673ab7; background-color: transparent; border: none; }");

    // 添加到布局
    layout->addWidget(m_nameLabel);
    layout->addWidget(separatorLine);
    layout->addWidget(m_tempLabel);
    layout->addWidget(m_humidityLabel);
    layout->addWidget(m_pressureLabel);
    layout->addWidget(m_lightLabel);
    layout->addWidget(m_waterVaporLabel);
    layout->addWidget(m_rainfallLabel);
    layout->addStretch();
}

void WeatherStationWidget::updateData(double temperature, double humidity, double pressure,
                                       double lightIntensity, double waterVapor, double rainfall)
{
    m_currentData.temperature = temperature;
    m_currentData.humidity = humidity;
    m_currentData.pressure = pressure;
    m_currentData.lightIntensity = lightIntensity;
    m_currentData.waterVapor = waterVapor;
    m_currentData.rainfall = rainfall;

    m_tempLabel->setText(QString::fromUtf8("温度: %1°C").arg(temperature, 0, 'f', 1));
    m_humidityLabel->setText(QString::fromUtf8("湿度: %1%").arg(humidity, 0, 'f', 1));
    m_pressureLabel->setText(QString::fromUtf8("气压: %1hPa").arg(pressure, 0, 'f', 1));
    m_lightLabel->setText(QString::fromUtf8("光强: %1lux").arg(lightIntensity, 0, 'f', 0));
    m_waterVaporLabel->setText(QString::fromUtf8("水汽: %1g/m³").arg(waterVapor, 0, 'f', 2));
    m_rainfallLabel->setText(QString::fromUtf8("雨量: %1mm").arg(rainfall, 0, 'f', 1));
}

void WeatherStationWidget::updateBME280Data(const struct bme280_data &data)
{
    m_currentData.temperature = data.temperature;
    m_currentData.humidity = data.humidity;
    m_currentData.pressure = data.pressure;

    m_tempLabel->setText(QString::fromUtf8("温度: %1°C").arg(data.temperature, 0, 'f', 1));
    m_humidityLabel->setText(QString::fromUtf8("湿度: %1%").arg(data.humidity, 0, 'f', 1));
    m_pressureLabel->setText(QString::fromUtf8("气压: %1hPa").arg(data.pressure, 0, 'f', 1));

    qDebug() << "Station" << m_stationName << "BME280 updated: T=" << data.temperature
             << "°C, H=" << data.humidity << "%, P=" << data.pressure << "hPa";
}

void WeatherStationWidget::updateLightRainData(const struct lightrain_data &data)
{
    m_currentData.lightIntensity = data.light_intensity;
    m_currentData.rainfall = data.rainfall;

    m_lightLabel->setText(QString::fromUtf8("光强: %1lux").arg(data.light_intensity, 0, 'f', 1));
    // 注意：这里雨量数据是百分比，显示为百分比
    m_rainfallLabel->setText(QString::fromUtf8("雨量: %1%").arg(data.rainfall));

    //qDebug() << "Station" << m_stationName << "LightRain updated: Lux=" << data.light_intensity
    //        << "lx, Rain=" << data.rainfall << "%";
}

void WeatherStationWidget::updateGPSData(const struct gps_data &data)
{
    // GPS数据显示为tooltip
    m_currentData.gpsInfo = QString("GPS: %1,%2").arg(data.latitude, 0, 'f', 5).arg(data.longitude, 0, 'f', 5);

    this->setToolTip(QString("节点ID: %1\nGPS: %2, %3\n海拔: %4m\n卫星数: %5\nUTC: %6")
                    .arg(data.node_id)
                    .arg(data.latitude, 0, 'f', 5)
                    .arg(data.longitude, 0, 'f', 5)
                    .arg(data.altitude, 0, 'f', 1)
                    .arg(data.satellites)
                    .arg(data.utc));

    //qDebug() << "Station" << m_stationName << "GPS updated: Lat=" << data.latitude
    //         << ", Lon=" << data.longitude << ", Alt=" << data.altitude << "m";
}

void WeatherStationWidget::setHighlighted(bool highlighted)
{
    m_isHighlighted = highlighted;

    if (highlighted) {
        this->setStyleSheet("WeatherStationWidget { "
                           "background-color: #fff3e0; "  // 橙色高亮背景
                           "border: 2px solid #ff9800; "  // 橙色边框
                           "border-radius: 5px; "
                           "margin: 3px; }");
    } else {
        this->setStyleSheet("WeatherStationWidget { "
                           "background-color: #e3f2fd; "  // 默认淡蓝色背景
                           "border: none; "
                           "border-radius: 5px; "
                           "margin: 3px; }");
    }
}

// AlertStationWidget 类的实现
AlertStationWidget::AlertStationWidget(const QString& stationName, QWidget *parent)
    : QFrame(parent), m_stationName(stationName)
{
    setupUI();
}

void AlertStationWidget::setupUI()
{
    // 设置框架样式
    this->setFrameStyle(QFrame::StyledPanel | QFrame::Raised);
    this->setFixedHeight(60);
    this->setStyleSheet("AlertStationWidget { "
                       "background-color: #ffffff; "
                       "border: 1px solid #e0e0e0; "
                       "border-radius: 6px; "
                       "margin: 1px; }");

    // 创建水平布局
    QHBoxLayout *mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(6, 4, 6, 4);
    mainLayout->setSpacing(6);

    // 左侧状态图标
    m_statusIconLabel = new QLabel(QString::fromUtf8("●"));
    m_statusIconLabel->setFixedSize(16, 16);
    m_statusIconLabel->setAlignment(Qt::AlignCenter);
    m_statusIconLabel->setStyleSheet("QLabel { "
                                    "font-size: 14px; "
                                    "font-weight: bold; "
                                    "color: #757575; }");

    // 右侧信息区域
    QVBoxLayout *infoLayout = new QVBoxLayout();
    infoLayout->setContentsMargins(0, 0, 0, 0);
    infoLayout->setSpacing(2);

    // 站点名称
    m_nameLabel = new QLabel(m_stationName);
    m_nameLabel->setStyleSheet("QLabel { "
                              "font-size: 11px; "
                              "font-weight: bold; "
                              "color: #1e3a8a; }");

    // 告警类型
    m_alertTypeLabel = new QLabel("");
    m_alertTypeLabel->setStyleSheet("QLabel { "
                                   "font-size: 9px; "
                                   "color: #666666; }");

    // 告警信息
    m_alertMessageLabel = new QLabel("");
    m_alertMessageLabel->setStyleSheet("QLabel { "
                                      "font-size: 9px; "
                                      "color: #d32f2f; }");
    m_alertMessageLabel->setWordWrap(true);

    // 时间标签
    m_timeLabel = new QLabel("");
    m_timeLabel->setStyleSheet("QLabel { "
                              "font-size: 8px; "
                              "color: #999999; }");

    // 添加到信息布局
    infoLayout->addWidget(m_nameLabel);
    infoLayout->addWidget(m_alertTypeLabel);
    infoLayout->addWidget(m_alertMessageLabel);
    infoLayout->addStretch();
    infoLayout->addWidget(m_timeLabel);

    // 添加到主布局
    mainLayout->addWidget(m_statusIconLabel);
    mainLayout->addLayout(infoLayout);
}

void AlertStationWidget::updateAlertData(const QString& alertType, const QString& alertMessage, const QString& status)
{
    // 更新状态图标和颜色
    m_statusIconLabel->setText(getStatusIcon(status));
    m_statusIconLabel->setStyleSheet(QString("QLabel { "
                                            "font-size: 14px; "
                                            "font-weight: bold; "
                                            "color: %1; }").arg(getStatusColor(status)));

    // 更新告警信息
    if (status == QString::fromUtf8("正常")) {
        m_alertTypeLabel->setText("");
        m_alertMessageLabel->setText(QString::fromUtf8("运行正常"));
        m_alertMessageLabel->setStyleSheet("QLabel { font-size: 9px; color: #4caf50; }");
    } else if (status == QString::fromUtf8("离线")) {
        m_alertTypeLabel->setText("");
        m_alertMessageLabel->setText(alertMessage.isEmpty() ? QString::fromUtf8("站点离线") : alertMessage);
        m_alertMessageLabel->setStyleSheet("QLabel { font-size: 9px; color: #757575; }");
    } else {
        m_alertTypeLabel->setText(alertType.isEmpty() ? "" : QString("[%1]").arg(alertType));
        m_alertMessageLabel->setText(alertMessage);

        QString messageColor;
        if (status == QString::fromUtf8("告警")) {
            messageColor = "#ff9800";
        } else if (status == QString::fromUtf8("警报")) {
            messageColor = "#f44336";
        } else {
            messageColor = "#666666";
        }

        m_alertMessageLabel->setStyleSheet(QString("QLabel { font-size: 9px; color: %1; }").arg(messageColor));
    }

    // 更新时间
    m_timeLabel->setText(QDateTime::currentDateTime().toString("hh:mm:ss"));
}

QString AlertStationWidget::getStatusIcon(const QString& status)
{
    Q_UNUSED(status);
    return QString::fromUtf8("●");
}

QString AlertStationWidget::getStatusColor(const QString& status)
{
    if (status == QString::fromUtf8("正常")) {
        return "#4caf50"; // 绿色
    } else if (status == QString::fromUtf8("告警")) {
        return "#ff9800"; // 橙色
    } else if (status == QString::fromUtf8("警报")) {
        return "#f44336"; // 红色
    } else if (status == QString::fromUtf8("离线")) {
        return "#757575"; // 灰色
    } else {
        return "#757575"; // 默认灰色
    }
}
