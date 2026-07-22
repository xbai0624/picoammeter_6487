#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <QElapsedTimer>
#include <QFile>
#include <QMainWindow>
#include <QTextStream>
#include <QVector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;

class AcquisitionThread;
class PlotWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onStartStop();
    void onBrowseLogFile();
    void onNewReadings(QVector<double> times, QVector<double> currents);
    void onInstrumentInfo(QString idn);
    void onAcquisitionError(QString message);
    void onAcquisitionFinished();

private:
    QWidget *buildControlPanel();
    void refreshSerialPorts();
    void startAcquisition();
    void stopAcquisition();
    void setControlsEnabled(bool enabled);
    bool openLogFile();
    void closeLogFile();

    // controls
    QComboBox *m_busCombo = nullptr;
    QStackedWidget *m_connStack = nullptr;
    QSpinBox *m_boardSpin = nullptr;
    QSpinBox *m_addressSpin = nullptr;
    QComboBox *m_portCombo = nullptr;
    QComboBox *m_baudCombo = nullptr;
    QComboBox *m_modeCombo = nullptr;
    QComboBox *m_rangeCombo = nullptr;
    QDoubleSpinBox *m_nplcSpin = nullptr;
    QDoubleSpinBox *m_refreshSpin = nullptr;
    QDoubleSpinBox *m_windowSpin = nullptr;
    QCheckBox *m_logCheck = nullptr;
    QLineEdit *m_logPathEdit = nullptr;
    QPushButton *m_browseButton = nullptr;
    QPushButton *m_startButton = nullptr;

    // readouts
    QLabel *m_logFileLabel = nullptr;
    QLabel *m_idnLabel = nullptr;
    QLabel *m_valueLabel = nullptr;
    QLabel *m_rateLabel = nullptr;

    PlotWidget *m_plot = nullptr;
    AcquisitionThread *m_thread = nullptr;

    // logging
    QFile m_logFile;
    QTextStream m_logStream;
    QElapsedTimer m_flushTimer;

    // rate estimate
    qint64 m_totalReadings = 0;
    QElapsedTimer m_rateTimer;
    qint64 m_rateBaseCount = 0;
};

#endif // MAIN_WINDOW_H
