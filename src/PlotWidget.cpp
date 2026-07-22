#include "PlotWidget.h"

#include <QPainter>
#include <algorithm>
#include <cmath>

PlotWidget::PlotWidget(QWidget *parent) : QWidget(parent)
{
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor(20, 24, 30));
    setPalette(pal);
}

void PlotWidget::addPoints(const QVector<double> &times, const QVector<double> &currents)
{
    const int n = std::min(times.size(), currents.size());
    for (int k = 0; k < n; ++k) {
        m_t.append(times[k]);
        m_i.append(currents[k]);
    }
    trim();
    update();
}

void PlotWidget::clearData()
{
    m_t.clear();
    m_i.clear();
    update();
}

void PlotWidget::setTimeWindow(double seconds)
{
    m_window = std::max(0.1, seconds);
    update();
}

void PlotWidget::trim()
{
    if (m_t.size() > m_maxPoints) {
        const int drop = m_t.size() - m_maxPoints;
        m_t.remove(0, drop);
        m_i.remove(0, drop);
    }
}

QString PlotWidget::formatCurrent(double amps, int precision)
{
    struct Prefix { double scale; const char *unit; };
    static const Prefix prefixes[] = {
        {1e-12, "pA"}, {1e-9, "nA"}, {1e-6, "uA"}, {1e-3, "mA"}, {1.0, "A"}};
    const double a = std::fabs(amps);
    const Prefix *best = &prefixes[4];
    for (const Prefix &p : prefixes) {
        if (a < p.scale * 1000.0) {
            best = &p;
            break;
        }
    }
    return QString::number(amps / best->scale, 'f', precision) + ' ' +
           QString::fromLatin1(best->unit);
}

void PlotWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const int marginL = 80, marginR = 12, marginT = 10, marginB = 32;
    const QRectF plot(marginL, marginT, width() - marginL - marginR,
                      height() - marginT - marginB);
    if (plot.width() < 10 || plot.height() < 10)
        return;

    p.setPen(QColor(90, 95, 105));
    p.drawRect(plot);

    if (m_t.isEmpty()) {
        p.setPen(QColor(150, 155, 165));
        p.drawText(plot, Qt::AlignCenter, tr("no data"));
        return;
    }

    // Visible time range: last m_window seconds.
    const double tMax = m_t.last();
    const double tMin = tMax - m_window;
    const auto firstIt = std::lower_bound(m_t.cbegin(), m_t.cend(), tMin);
    const int first = int(firstIt - m_t.cbegin());
    const int count = m_t.size() - first;
    if (count < 1)
        return;

    // Y autoscale over the visible range, with 10% padding.
    double yMin = m_i[first], yMax = m_i[first];
    for (int k = first; k < m_i.size(); ++k) {
        yMin = std::min(yMin, m_i[k]);
        yMax = std::max(yMax, m_i[k]);
    }
    double pad = (yMax - yMin) * 0.1;
    if (pad <= 0.0)
        pad = (std::fabs(yMax) > 0.0 ? std::fabs(yMax) * 0.1 : 1e-12);
    yMin -= pad;
    yMax += pad;

    const auto xPix = [&](double t) {
        return plot.left() + (t - tMin) / m_window * plot.width();
    };
    const auto yPix = [&](double i) {
        return plot.bottom() - (i - yMin) / (yMax - yMin) * plot.height();
    };

    // Grid + axis labels.
    p.setPen(QColor(55, 60, 70));
    QFont f = p.font();
    f.setPointSizeF(f.pointSizeF() * 0.9);
    p.setFont(f);
    const int nYTicks = 5;
    for (int k = 0; k <= nYTicks; ++k) {
        const double v = yMin + (yMax - yMin) * k / nYTicks;
        const double y = yPix(v);
        p.setPen(QColor(55, 60, 70));
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        p.setPen(QColor(170, 175, 185));
        p.drawText(QRectF(0, y - 9, marginL - 6, 18),
                   Qt::AlignRight | Qt::AlignVCenter, formatCurrent(v));
    }
    const int nXTicks = 6;
    for (int k = 0; k <= nXTicks; ++k) {
        const double t = tMin + m_window * k / nXTicks;
        const double x = xPix(t);
        p.setPen(QColor(55, 60, 70));
        p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        p.setPen(QColor(170, 175, 185));
        p.drawText(QRectF(x - 40, plot.bottom() + 4, 80, 18), Qt::AlignHCenter,
                   QString::number(t, 'f', m_window < 10 ? 1 : 0) + " s");
    }

    // Trace. Decimate whenever the data is dense relative to the pixels it
    // actually covers (early in a run all points sit in a narrow strip, so
    // comparing against the full widget width would stroke thousands of
    // overlapping sub-pixel segments and stall the GUI thread).
    p.setClipRect(plot);
    p.setPen(QPen(QColor(80, 200, 255), 1.2));
    const double spanPx =
        std::max(1.0, (m_t.last() - m_t[first]) / m_window * plot.width());
    if (count <= 2.0 * spanPx) {
        // Independent segments, not one connected QPainterPath: stroking a
        // long self-intersecting path is pathologically slow in Qt's raster
        // engine (hundreds of ms for <1000 noisy points).
        QVector<QLineF> segments;
        segments.reserve(count - 1);
        for (int k = first + 1; k < m_t.size(); ++k)
            segments.append(QLineF(xPix(m_t[k - 1]), yPix(m_i[k - 1]),
                                   xPix(m_t[k]), yPix(m_i[k])));
        p.drawLines(segments);
    } else {
        // One min/max vertical segment per pixel column, seeded with the
        // previous column's last value so the trace stays connected.
        int k = first;
        double carryY = 0;
        bool haveCarry = false;
        for (int col = 0; col < int(plot.width()); ++col) {
            const double tHi = tMin + m_window * (col + 1) / plot.width();
            double lo = 0, hi = 0;
            bool any = false;
            while (k < m_t.size() && m_t[k] <= tHi) {
                if (!any) {
                    lo = hi = m_i[k];
                    any = true;
                } else {
                    lo = std::min(lo, m_i[k]);
                    hi = std::max(hi, m_i[k]);
                }
                ++k;
            }
            if (any) {
                if (haveCarry) {
                    lo = std::min(lo, carryY);
                    hi = std::max(hi, carryY);
                }
                carryY = m_i[k - 1];
                haveCarry = true;
                const double x = plot.left() + col + 0.5;
                p.drawLine(QPointF(x, yPix(lo)), QPointF(x, yPix(hi)));
            }
        }
    }
    p.setClipping(false);
}
