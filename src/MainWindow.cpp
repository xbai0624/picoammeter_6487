#include "MainWindow.h"
#include "AcquisitionThread.h"
#include "PlotWidget.h"

#include <QCoreApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSerialPortInfo>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle(tr("Keithley 6485/6487 Picoammeter"));

    m_plot = new PlotWidget(this);

    auto *central = new QWidget(this);
    auto *layout = new QHBoxLayout(central);
    layout->addWidget(buildControlPanel());
    layout->addWidget(m_plot, 1);
    setCentralWidget(central);

    m_thread = new AcquisitionThread(this);
    connect(m_thread, &AcquisitionThread::newReadings, this, &MainWindow::onNewReadings);
    connect(m_thread, &AcquisitionThread::instrumentInfo, this, &MainWindow::onInstrumentInfo);
    connect(m_thread, &AcquisitionThread::errorOccurred, this, &MainWindow::onAcquisitionError);
    connect(m_thread, &AcquisitionThread::finishedNormally, this,
            &MainWindow::onAcquisitionFinished);
    // Live refresh-rate adjustment, also effective while acquiring.
    connect(m_refreshSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this](double s) { m_thread->setRefreshSeconds(s); });

    refreshSerialPorts();
    statusBar()->showMessage(tr("Idle"));
    resize(1100, 600);

    // "--autostart" begins acquisition immediately (useful for testing/scripting);
    // "--continuous" selects continuous mode first.
    if (QCoreApplication::arguments().contains(QStringLiteral("--continuous")))
        m_modeCombo->setCurrentIndex(m_modeCombo->findData(
            int(AcquisitionThread::Mode::Continuous)));
    if (QCoreApplication::arguments().contains(QStringLiteral("--autostart")))
        QMetaObject::invokeMethod(this, [this] { startAcquisition(); },
                                  Qt::QueuedConnection);
}

MainWindow::~MainWindow() = default;

QWidget *MainWindow::buildControlPanel()
{
    auto *panel = new QWidget(this);
    panel->setFixedWidth(320);
    auto *v = new QVBoxLayout(panel);

    // --- Connection ---
    auto *connBox = new QGroupBox(tr("Connection"), panel);
    auto *connLayout = new QVBoxLayout(connBox);

    m_busCombo = new QComboBox(connBox);
    m_busCombo->addItem(tr("GPIB (IEEE-488)"), int(AcquisitionThread::Bus::Gpib));
    m_busCombo->addItem(tr("RS-232 serial"), int(AcquisitionThread::Bus::Serial));
    connLayout->addWidget(m_busCombo);

    m_connStack = new QStackedWidget(connBox);

    auto *gpibPage = new QWidget(m_connStack);
    auto *gpibForm = new QFormLayout(gpibPage);
    gpibForm->setContentsMargins(0, 0, 0, 0);
    m_boardSpin = new QSpinBox(gpibPage);
    m_boardSpin->setRange(0, 7);
    m_addressSpin = new QSpinBox(gpibPage);
    m_addressSpin->setRange(0, 30);
    m_addressSpin->setValue(22); // 6487 factory default
    gpibForm->addRow(tr("Board index"), m_boardSpin);
    gpibForm->addRow(tr("Address"), m_addressSpin);
    m_connStack->addWidget(gpibPage);

    auto *serialPage = new QWidget(m_connStack);
    auto *serialForm = new QFormLayout(serialPage);
    serialForm->setContentsMargins(0, 0, 0, 0);
    m_portCombo = new QComboBox(serialPage);
    m_portCombo->setEditable(true); // allow typing a port name by hand
    m_baudCombo = new QComboBox(serialPage);
    for (int baud : {9600, 19200, 38400, 57600})
        m_baudCombo->addItem(QString::number(baud), baud);
    m_baudCombo->setCurrentIndex(m_baudCombo->count() - 1); // 57600
    m_baudCombo->setToolTip(tr("Must match the instrument's MENU -> RS-232 "
                               "baud rate setting."));
    serialForm->addRow(tr("Port"), m_portCombo);
    serialForm->addRow(tr("Baud rate"), m_baudCombo);
    m_connStack->addWidget(serialPage);

    connLayout->addWidget(m_connStack);
    connect(m_busCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                m_connStack->setCurrentIndex(index);
                if (index == 1)
                    refreshSerialPorts();
            });
    v->addWidget(connBox);

    // --- Measurement ---
    auto *measBox = new QGroupBox(tr("Measurement"), panel);
    auto *measForm = new QFormLayout(measBox);

    m_modeCombo = new QComboBox(measBox);
    m_modeCombo->addItem(tr("Continuous (batched readings)"),
                         int(AcquisitionThread::Mode::Continuous));
    m_modeCombo->addItem(tr("Fast burst (~1000 rdg/s, gaps)"), int(AcquisitionThread::Mode::Burst));
    m_modeCombo->setToolTip(tr("Fast burst: instrument buffer, maximum rate.\n"
                               "Continuous: batched READ? triggers - near gap-free,\n"
                               "only ~10 ms between batches."));

    m_rangeCombo = new QComboBox(measBox);
    const struct { const char *label; double amps; } ranges[] = {
        {"2 nA", 2e-9},   {"20 nA", 2e-8},  {"200 nA", 2e-7},
        {"2 uA", 2e-6},   {"20 uA", 2e-5},  {"200 uA", 2e-4},
        {"2 mA", 2e-3},   {"20 mA", 2e-2},  {"Auto (slow)", -1.0},
    };
    for (const auto &r : ranges)
        m_rangeCombo->addItem(QString::fromLatin1(r.label), r.amps);
    m_rangeCombo->setCurrentIndex(3); // 2 uA

    m_nplcSpin = new QDoubleSpinBox(measBox);
    m_nplcSpin->setRange(0.01, 10.0);
    m_nplcSpin->setDecimals(2);
    m_nplcSpin->setSingleStep(0.01);
    m_nplcSpin->setValue(0.01); // fastest integration
    m_nplcSpin->setToolTip(tr("Integration time in power-line cycles.\n"
                              "0.01 = fastest (~1000 rdg/s in burst mode), 1+ = quieter."));

    m_refreshSpin = new QDoubleSpinBox(measBox);
    m_refreshSpin->setRange(0.01, 3.0);
    m_refreshSpin->setDecimals(2);
    m_refreshSpin->setSingleStep(0.01);
    m_refreshSpin->setValue(0.05);
    m_refreshSpin->setSuffix(tr(" s"));
    m_refreshSpin->setToolTip(
        tr("Display update interval. Adjustable while acquiring.\n"
           "In burst mode the burst size is derived automatically from the\n"
           "measured reading rate to match this interval."));

    measForm->addRow(tr("Mode"), m_modeCombo);
    measForm->addRow(tr("Range"), m_rangeCombo);
    measForm->addRow(tr("NPLC"), m_nplcSpin);
    measForm->addRow(tr("Refresh"), m_refreshSpin);

    m_displayCheck = new QCheckBox(tr("Keep instrument display on"), measBox);
    m_displayCheck->setChecked(false);
    m_displayCheck->setToolTip(tr("Leave the front panel display enabled during\n"
                                  "acquisition. Slightly reduces the maximum\n"
                                  "reading rate; irrelevant at low sample rates."));
    measForm->addRow(QString(), m_displayCheck);
    v->addWidget(measBox);

    // --- Display ---
    auto *dispBox = new QGroupBox(tr("Display"), panel);
    auto *dispForm = new QFormLayout(dispBox);
    m_windowSpin = new QDoubleSpinBox(dispBox);
    m_windowSpin->setRange(1.0, 3600.0);
    m_windowSpin->setValue(30.0);
    m_windowSpin->setSuffix(tr(" s"));
    connect(m_windowSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), m_plot,
            &PlotWidget::setTimeWindow);
    dispForm->addRow(tr("Time window"), m_windowSpin);
    v->addWidget(dispBox);

    // --- Logging ---
    auto *logBox = new QGroupBox(tr("Logging"), panel);
    auto *logLayout = new QVBoxLayout(logBox);
    m_logCheck = new QCheckBox(tr("Write readings to text file"), logBox);
    m_logCheck->setChecked(true);
    auto *pathRow = new QHBoxLayout;
    m_logPathEdit = new QLineEdit(logBox);
    m_logPathEdit->setText(QDir::toNativeSeparators(QDir::currentPath()));
    m_logPathEdit->setToolTip(tr("Directory for the log files. Each Start creates a new\n"
                                 "pico6487_<date>_<time>.txt file in this directory."));
    m_browseButton = new QPushButton(tr("..."), logBox);
    m_browseButton->setFixedWidth(36);
    connect(m_browseButton, &QPushButton::clicked, this, &MainWindow::onBrowseLogFile);
    pathRow->addWidget(m_logPathEdit);
    pathRow->addWidget(m_browseButton);
    logLayout->addWidget(m_logCheck);
    logLayout->addLayout(pathRow);
    m_logFileLabel = new QLabel(tr("not logging"), logBox);
    m_logFileLabel->setWordWrap(true);
    m_logFileLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_logFileLabel->setStyleSheet(QStringLiteral("color: gray; font-size: 11px;"));
    logLayout->addWidget(m_logFileLabel);
    v->addWidget(logBox);

    // --- Readout ---
    m_valueLabel = new QLabel(QStringLiteral("--"), panel);
    QFont big = m_valueLabel->font();
    big.setPointSizeF(big.pointSizeF() * 2.2);
    big.setBold(true);
    m_valueLabel->setFont(big);
    m_valueLabel->setAlignment(Qt::AlignCenter);
    m_rateLabel = new QLabel(tr("rate: --"), panel);
    m_rateLabel->setAlignment(Qt::AlignCenter);
    m_idnLabel = new QLabel(tr("not connected"), panel);
    m_idnLabel->setWordWrap(true);
    m_idnLabel->setStyleSheet(QStringLiteral("color: gray; font-size: 10px;"));
    v->addWidget(m_valueLabel);
    v->addWidget(m_rateLabel);
    v->addWidget(m_idnLabel);

    v->addStretch(1);

    m_startButton = new QPushButton(tr("Start"), panel);
    m_startButton->setMinimumHeight(40);
    connect(m_startButton, &QPushButton::clicked, this, &MainWindow::onStartStop);
    v->addWidget(m_startButton);

    return panel;
}

void MainWindow::refreshSerialPorts()
{
    const QString current = m_portCombo->currentText();
    m_portCombo->clear();
    for (const QSerialPortInfo &info : QSerialPortInfo::availablePorts())
        m_portCombo->addItem(info.portName());
    if (!current.isEmpty()) {
        const int idx = m_portCombo->findText(current);
        if (idx >= 0)
            m_portCombo->setCurrentIndex(idx);
        else
            m_portCombo->setEditText(current);
    }
}

void MainWindow::onStartStop()
{
    if (m_thread->isRunning())
        stopAcquisition();
    else
        startAcquisition();
}

void MainWindow::startAcquisition()
{
    if (m_logCheck->isChecked() && !openLogFile())
        return;

    AcquisitionThread::Parameters p;
    p.bus = AcquisitionThread::Bus(m_busCombo->currentData().toInt());
    p.boardIndex = m_boardSpin->value();
    p.gpibAddress = m_addressSpin->value();
    p.serialPort = m_portCombo->currentText().trimmed();
    p.baudRate = m_baudCombo->currentData().toInt();
    if (p.bus == AcquisitionThread::Bus::Serial && p.serialPort.isEmpty()) {
        QMessageBox::warning(this, tr("Connection"),
                             tr("Select or type a serial port name first."));
        closeLogFile();
        return;
    }
    p.mode = AcquisitionThread::Mode(m_modeCombo->currentData().toInt());
    p.rangeAmps = m_rangeCombo->currentData().toDouble();
    p.nplc = m_nplcSpin->value();
    p.refreshSeconds = m_refreshSpin->value();
    p.keepDisplayOn = m_displayCheck->isChecked();
    m_thread->setParameters(p);

    m_plot->clearData();
    m_totalReadings = 0;
    m_rateBaseCount = 0;
    m_rateTimer.start();

    m_thread->start();
    m_startButton->setText(tr("Stop"));
    setControlsEnabled(false);
    statusBar()->showMessage(m_logFile.isOpen()
                                 ? tr("Acquiring... logging to %1").arg(m_logFile.fileName())
                                 : tr("Acquiring..."));
}

void MainWindow::stopAcquisition()
{
    m_thread->requestStop();
    m_startButton->setEnabled(false);
    statusBar()->showMessage(tr("Stopping..."));
}

void MainWindow::setControlsEnabled(bool enabled)
{
    for (QWidget *w : {static_cast<QWidget *>(m_busCombo), static_cast<QWidget *>(m_portCombo),
                       static_cast<QWidget *>(m_baudCombo),
                       static_cast<QWidget *>(m_boardSpin), static_cast<QWidget *>(m_addressSpin),
                       static_cast<QWidget *>(m_modeCombo), static_cast<QWidget *>(m_rangeCombo),
                       static_cast<QWidget *>(m_nplcSpin), static_cast<QWidget *>(m_displayCheck),
                       static_cast<QWidget *>(m_logCheck), static_cast<QWidget *>(m_logPathEdit),
                       static_cast<QWidget *>(m_browseButton)})
        w->setEnabled(enabled);
}

bool MainWindow::openLogFile()
{
    // Every Start gets its own freshly timestamped file inside the chosen
    // directory; on a (same-second) name collision a _1, _2, ... suffix is
    // appended rather than overwriting.
    QDir dir(m_logPathEdit->text().trimmed().isEmpty()
                 ? QDir::currentPath()
                 : m_logPathEdit->text().trimmed());
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        QMessageBox::warning(this, tr("Log directory"),
                             tr("Log directory \"%1\" does not exist and cannot be created.")
                                 .arg(dir.path()));
        return false;
    }
    const QString stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_hhmmss"));
    QString path = dir.filePath(QStringLiteral("pico6487_%1.txt").arg(stamp));
    for (int n = 1; QFile::exists(path); ++n)
        path = dir.filePath(QStringLiteral("pico6487_%1_%2.txt").arg(stamp).arg(n));
    m_logFile.setFileName(path);
    if (!m_logFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Log file"),
                             tr("Cannot open \"%1\" for writing:\n%2")
                                 .arg(path, m_logFile.errorString()));
        return false;
    }
    m_logStream.setDevice(&m_logFile);
    m_logStream.setRealNumberNotation(QTextStream::ScientificNotation);
    m_logStream << "# Keithley 6487 current log\n"
                << "# Start: "
                << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << '\n'
                << "# Mode: " << m_modeCombo->currentText() << '\n'
                << "# Range: " << m_rangeCombo->currentText()
                << "  NPLC: " << m_nplcSpin->value() << '\n'
                << "# columns: elapsed_seconds  current_amps\n";
    m_flushTimer.start();
    m_logFileLabel->setText(tr("writing: %1").arg(path));
    return true;
}

void MainWindow::closeLogFile()
{
    if (m_logFile.isOpen()) {
        m_logStream.flush();
        m_logStream.setDevice(nullptr);
        m_logFile.close();
        m_logFileLabel->setText(tr("last file: %1").arg(m_logFile.fileName()));
    }
}

void MainWindow::onBrowseLogFile()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Choose log directory"), m_logPathEdit->text());
    if (!dir.isEmpty())
        m_logPathEdit->setText(QDir::toNativeSeparators(dir));
}

void MainWindow::onNewReadings(QVector<double> times, QVector<double> currents)
{
    m_plot->addPoints(times, currents);

    if (m_logFile.isOpen()) {
        const int n = qMin(times.size(), currents.size());
        for (int k = 0; k < n; ++k) {
            m_logStream << QString::asprintf("%.6f\t%.6e\n", times[k], currents[k]);
        }
        if (m_flushTimer.elapsed() > 1000) {
            m_logStream.flush();
            m_flushTimer.restart();
        }
    }

    if (!currents.isEmpty())
        m_valueLabel->setText(PlotWidget::formatCurrent(currents.last()));

    m_totalReadings += currents.size();
    if (m_rateTimer.elapsed() > 1000) {
        const double rate = double(m_totalReadings - m_rateBaseCount) /
                            (m_rateTimer.elapsed() / 1000.0);
        m_rateLabel->setText(tr("rate: %1 rdg/s   total: %2")
                                 .arg(rate, 0, 'f', 0)
                                 .arg(m_totalReadings));
        m_rateBaseCount = m_totalReadings;
        m_rateTimer.restart();
    }
}

void MainWindow::onInstrumentInfo(QString idn)
{
    m_idnLabel->setText(idn);
    statusBar()->showMessage(tr("Connected: %1").arg(idn), 5000);
}

void MainWindow::onAcquisitionError(QString message)
{
    statusBar()->showMessage(message);
    QMessageBox::warning(this, tr("Acquisition error"), message);
    onAcquisitionFinished();
}

void MainWindow::onAcquisitionFinished()
{
    if (m_thread->isRunning())
        m_thread->wait(5000);
    closeLogFile();
    m_startButton->setText(tr("Start"));
    m_startButton->setEnabled(true);
    setControlsEnabled(true);
    if (statusBar()->currentMessage() == tr("Stopping..."))
        statusBar()->showMessage(tr("Idle"));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_thread->isRunning()) {
        m_thread->requestStop();
        m_thread->wait(5000);
    }
    closeLogFile();
    event->accept();
}
