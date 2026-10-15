// The all-tags tool's list of tags (Kainote's Hydra): each with its sliders' range, step and
// digits, its default values (one to four sliders), where Insert puts it and the change mode it
// starts in; kept in the settings, edited in HydraDialog.
#pragma once

#include <QDialog>
#include <QList>
#include <QString>
#include <vector>

class QListWidget;
class QLineEdit;
class QDoubleSpinBox;
class QSpinBox;
class QComboBox;

namespace hydra {

struct Tag {
    QString name, tag; // tag without the backslash
    double min = 0, max = 100, step = 1;
    int digits = 1;
    QList<double> values{0}; // the sliders' defaults, one to four
    bool atStart = false;    // Insert puts it at the line's start, not at the cursor
    int mode = 0;            // the change mode picked with it, of modes()
    bool operator==(const Tag &) const = default;
};

// The change modes, in VisualTools' list order (Session::hydra's numbers), untranslated
// (VisualTools context)
const QList<const char *> &modes();
std::vector<Tag> defaults();
std::vector<Tag> load(); // the user's list, else the defaults
void save(const std::vector<Tag> &tags);
// How many values a tag has whatever it lists: alpha one, a colour three; 0 for the others
int fixedCount(const QString &tag);
int count(const Tag &t); // fixedCount, else as many as it lists

} // namespace hydra

class HydraDialog : public QDialog {
    Q_OBJECT
public:
    HydraDialog(QWidget *parent, std::vector<hydra::Tag> tags, int current);
    const std::vector<hydra::Tag> &tags() const { return m_tags; }
    int current() const;
    void accept() override; // checks every tag first

private:
    void fill();           // the list's names
    void showTag(int i);   // the form from tag i
    void store();          // the form into the shown tag
    std::vector<hydra::Tag> m_tags;
    int m_shown = -1;
    bool m_loading = false;
    QListWidget *m_list;
    QLineEdit *m_name, *m_tag;
    QDoubleSpinBox *m_min, *m_max, *m_step, *m_values[4];
    QSpinBox *m_digits, *m_count;
    QComboBox *m_at, *m_mode;
};
