// Shift Times panel docked right of the grid; a header button flips it to
// the timing post-processor (Kainote layout).
#pragma once

#include <QScrollArea>

class TabPanel;
class TimeEdit;
class QCheckBox;
class QComboBox;
class QRadioButton;
class QLineEdit;
class QSpinBox;
class QStackedWidget;
class QPushButton;
class QSettings;

class ShiftPanel : public QScrollArea {
    Q_OBJECT
public:
    explicit ShiftPanel(TabPanel *tab);
    void shift();
    void postProcess();
    void run(); // the page shown: shift, or post-process (Kainote's one command for both)

protected:
    void enterEvent(QEnterEvent *) override;

private:
    void save() const;
    void load();
    void store(QSettings &st) const; // shift-page values, also used for profiles
    void restore(QSettings &st);
    void updateEnabled();            // frames / marker options need video or audio
    std::vector<size_t> rows(int which, const QString &styles) const;

    TabPanel *m_tab;
    QStackedWidget *m_stack;
    QPushButton *m_mode;
    // shift
    QComboBox *m_profiles;
    TimeEdit *m_time;
    QRadioButton *m_forward, *m_backward;
    QCheckBox *m_frames, *m_tagTimes;
    QCheckBox *m_markerToVideo, *m_markerToAudio;
    QRadioButton *m_byStart, *m_byEnd;
    QComboBox *m_lines, *m_which, *m_endFix;
    QLineEdit *m_styles;
    // post processor
    QComboBox *m_ppLines;
    QLineEdit *m_ppStyles;
    QCheckBox *m_leadInOn, *m_leadOutOn, *m_contOn, *m_kfOn;
    QSpinBox *m_leadIn, *m_leadOut, *m_contStart, *m_contEnd, *m_contOverlap, *m_kfBs, *m_kfAs, *m_kfBe, *m_kfAe;
};
