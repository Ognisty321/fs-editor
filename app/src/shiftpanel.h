// Shift Times panel docked right of the grid; a header button flips it to
// the timing post-processor (Kainote layout).
#pragma once

#include <QScrollArea>

class TabPanel;
class TimeEdit;
class QCheckBox;
class QRadioButton;
class QLineEdit;
class QSpinBox;
class QStackedWidget;
class QPushButton;
class QDoubleSpinBox;

class ShiftPanel : public QScrollArea {
    Q_OBJECT
public:
    explicit ShiftPanel(TabPanel *tab);
    void shift();
    void postProcess();

private:
    void save() const;
    void load();
    int whichLines() const;

    TabPanel *m_tab;
    QStackedWidget *m_stack;
    QPushButton *m_mode;
    // shift
    TimeEdit *m_time;
    QRadioButton *m_forward, *m_backward;
    QCheckBox *m_frames, *m_tagTimes;
    QCheckBox *m_byVideo, *m_markerToVideo, *m_markerToAudio;
    QRadioButton *m_byStart, *m_byEnd;
    QRadioButton *m_lines[6];
    QLineEdit *m_styles;
    QRadioButton *m_which[3];
    QRadioButton *m_endFix[3];
    // post processor
    QCheckBox *m_leadInOn, *m_leadOutOn, *m_contOn, *m_kfOn;
    QSpinBox *m_leadIn, *m_leadOut, *m_contStart, *m_kfBs, *m_kfAs, *m_kfBe, *m_kfAe;
    QDoubleSpinBox *m_bias;
};
