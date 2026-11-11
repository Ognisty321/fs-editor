// Aegisub's kanji timer.
#pragma once

#include "bridge.h"

#include <QDialog>
#include <QPointer>
#include <vector>

class TabPanel;
class QComboBox;
class QCheckBox;
class QLabel;

// Aegisub's kanji timer: groups a romaji karaoke line's syllables with the
// characters of a kanji line and writes the timing onto the kanji line.
class KanjiTimerDialog : public QDialog {
    Q_OBJECT
public:
    KanjiTimerDialog(QWidget *parent, TabPanel *tab);
    void done(int r) override;

protected:
    void keyPressEvent(QKeyEvent *e) override;

private:
    int find(int from, const QString &style, int step) const;
    void reset();
    void refresh();
    void link();
    void acceptLine();
    QPointer<TabPanel> m_tab;
    rust::Box<fsb::KanjiTimer> m_kt;
    QComboBox *m_src, *m_dst;
    QCheckBox *m_interpolate;
    QLabel *m_srcView, *m_dstView;
    int m_srcRow = -1, m_dstRow = -1;
    std::vector<std::pair<size_t, QString>> m_changes;
};
