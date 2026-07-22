#ifndef PLOT_WIDGET_H
#define PLOT_WIDGET_H

#include <QVector>
#include <QWidget>

// Lightweight scrolling current-vs-time plot (no external dependencies).
// Stores up to maxPoints recent samples; older ones are dropped.
class PlotWidget : public QWidget
{
    Q_OBJECT
public:
    explicit PlotWidget(QWidget *parent = nullptr);

    void addPoints(const QVector<double> &times, const QVector<double> &currents);
    void clearData();

    void setTimeWindow(double seconds); // visible history width
    double timeWindow() const { return m_window; }

    static QString formatCurrent(double amps, int precision = 3);

    QSize minimumSizeHint() const override { return {400, 250}; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void trim();

    QVector<double> m_t; // monotonic, seconds
    QVector<double> m_i; // amps
    double m_window = 30.0;
    int m_maxPoints = 500000;
};

#endif // PLOT_WIDGET_H
