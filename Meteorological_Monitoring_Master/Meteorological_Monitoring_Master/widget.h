#ifndef WIDGET_H
#define WIDGET_H

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QFrame>
#include <QDateTime>
#include <QTimer>
#include <QLineEdit>
#include <QSpinBox>
#include <QGroupBox>
#include <QCheckBox>
#include <QMessageBox>
#include <QTextEdit>
#include <random>

extern "C" {
#include "shared_data.h"
}

#include <sys/shm.h>
#include <sys/ipc.h>

QT_BEGIN_NAMESPACE
QT_END_NAMESPACE

class WeatherStationWidget;
class AlertStationWidget;

class Widget : public QWidget
{
    Q_OBJECT

public:
    Widget(QWidget *parent = nullptr);
    ~Widget();

private slots:
    //void updateWeatherData(); // 用于更新随机气象数据（测试用）
    void checkSharedMemoryUpdate(); // 检查共享内存数据更新
    void updateTimeDisplay(); // 更新时间显示
    void showSystemInfo(); // 显示系统信息
    void updateSystemInfoDisplay(); // 更新系统信息显示
    void updateDataFromSharedMemory(); // 从共享内存更新数据

private:
    void setupUI();
    void setupTopWidget();
    void setupBottomWidget();
    void setupLeftWidget();  // 设置左侧网格布局
    void setuprightWidget(); // 设置右侧垂直布局
    void setupMapWidget();   // 设置地图widget
    void setupAlertScrollArea(); // 设置告警滚动区域
    void setupLegendWidget(); // 设置图例widget
    void setupSystemInfoWidget(); // 设置系统信息widget

    // 共享内存相关
    bool initSharedMemory();
    void cleanupSharedMemory();
    void updateConnectionStatus();

    // 数据更新相关 - 修改和新增
    void updateStationWithBME280Data(const struct bme280_data &data, int stationIndex);
    void updateStationWithLightRainData(const struct lightrain_data &data, int stationIndex);
    void updateStationWithGPSData(const struct gps_data &data, int stationIndex);
    void updateAlertWithLatestData(int stationIndex);
    void simulateRandomDataForOtherStations(int excludeIndex);

    // 新增：确保某 nodeId 的控件已创建并显示
    void ensureWidgetsForNode(int nodeId);


    // 通过node_id获取站点名称
    QString getStationNameFromNodeId(int nodeId);
    // 主布局相关
    QVBoxLayout *m_mainLayout;
    // 上下两个主要区域
    QWidget *m_topWidget;
    QWidget *m_bottomWidget;
    // 上部分控件 - 标题、时间和控制按钮
    QHBoxLayout *m_topLayout;
    QLabel *m_titleLabel;
    QLabel *m_timeLabel;
    QPushButton *m_infoButton;
    QLabel *m_connectionStatusLabel;
    // 下部分控件
    QHBoxLayout *m_bottomLayout;
    QWidget *m_leftWidget;   // 左边区域 - 气象数据显示
    QWidget *m_rightWidget;  // 右边区域 - 系统信息和告警
    // 左侧网格布局相关
    QGridLayout *m_gridLayout;
    QList<WeatherStationWidget*> m_stationWidgets; // 气象站widget列表
    // 右侧垂直布局相关
    QVBoxLayout *m_rightLayout;
    QWidget *m_upperWidget;   // 上区域 - 系统信息
    QWidget *m_middleWidget;  // 中间区域 - 告警信息
    QWidget *m_lowerWidget;   // 下区域 - 图例说明
    // 系统信息相关
    QTextEdit *m_systemInfoText;
    // Scroll Area相关
    QScrollArea *m_scrollArea;
    QWidget *m_scrollContentWidget;
    QVBoxLayout *m_scrollLayout;
    QList<AlertStationWidget*> m_alertWidgets; // 告警信息widget列表
    // 定时器
    QTimer *m_timeUpdateTimer; // 时间更新定时器
    QTimer *m_dataCheckTimer;  // 数据检查定时器
    QTimer *m_updateSystemInfoTimer;

    // 共享内存相关
    struct shared_weather_data *m_sharedData;
    int m_shmId;
    bool m_sharedMemoryValid;
    uint32_t m_lastUpdateCounter; // 上次更新计数器
    // 数据相关
    int m_currentStationIndex; // 当前显示真实数据的站点索引
    bool m_useRealData; // 是否使用真实数据
    int nodeId;

    //新增：nodeId -> 控件映射
    QMap<int,WeatherStationWidget*> m_stationByNodeId;
    QMap<int,AlertStationWidget*> m_alertByNodeId;

    // 新增：网格位置游标
    int m_nextGridRow = 0;
    int m_nextGridCol = 0;
    static constexpr int kGridCols = 3;// 左侧 3 列
};




// 气象站数据显示widget
class WeatherStationWidget : public QFrame
{
    Q_OBJECT

public:
    explicit WeatherStationWidget(const QString& stationName, QWidget *parent = nullptr);
    void updateData(double temperature, double humidity, double pressure,
                   double lightIntensity, double waterVapor, double rainfall);

    // 新增：分别更新不同类型的传感器数据
    void updateBME280Data(const struct bme280_data &data);
    void updateLightRainData(const struct lightrain_data &data);
    void updateGPSData(const struct gps_data &data);
    void setStationName(const QString &stationName);
    void setHighlighted(bool highlighted); // 设置高亮显示

private:
    void setupUI();

    QString m_stationName;
    QLabel *m_nameLabel;
    QLabel *m_tempLabel;
    QLabel *m_humidityLabel;
    QLabel *m_pressureLabel;
    QLabel *m_lightLabel;
    QLabel *m_waterVaporLabel;
    QLabel *m_rainfallLabel;
    QLabel *m_timeLabel;
    bool m_isHighlighted;

    // 新增：存储当前显示的数据，用于部分更新
    struct {
        double temperature = 0.0;
        double humidity = 0.0;
        double pressure = 1013.0;
        double lightIntensity = 25000.0;
        double waterVapor = 12.5;
        double rainfall = 0.0;
        QString gpsInfo = "";
    } m_currentData;
};

// 告警数据显示widget
class AlertStationWidget : public QFrame
{
    Q_OBJECT

public:
    explicit AlertStationWidget(const QString& stationName, QWidget *parent = nullptr);
    void updateAlertData(const QString& alertType, const QString& alertMessage, const QString& status);

private:
    void setupUI();
    QString getStatusIcon(const QString& status);
    QString getStatusColor(const QString& status);

    QString m_stationName;
    QLabel *m_nameLabel;
    QLabel *m_statusIconLabel;
    QLabel *m_alertTypeLabel;
    QLabel *m_alertMessageLabel;
    QLabel *m_timeLabel;
};

#endif // WIDGET_H
