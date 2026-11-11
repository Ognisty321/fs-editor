#include "styleeditor.h"

#include "colorpicker.h"
#include "dialogutil.h"
#include "icons.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QGridLayout>
#include <QGroupBox>
#include <QHash>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QRawFont>
#include <QSettings>
#include <QSpinBox>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <optional>

namespace {
QString assColorString(const QColor &c) {
    return QString::asprintf("&H%02X%02X%02X%02X", 255 - c.alpha(), c.blue(), c.green(), c.red());
}
} // namespace

// Kainote's font filter: whether the family has a glyph for every one of the characters
static bool fontHasAll(const QString &family, const QString &chars) {
    static QHash<QString, bool> known; // a family looked at once per filter text
    const QString key = family + '\n' + chars;
    if (const auto it = known.constFind(key); it != known.cend()) return *it;
    const QRawFont raw = QRawFont::fromFont(QFont(family));
    bool all = raw.isValid();
    for (const char32_t c : chars.toUcs4())
        if (all && !QChar::isSpace(c)) all = raw.supportsCharacter(c);
    return known[key] = all;
}

// Kainote's font catalogs: named lists of font families in the settings; a font may be in several
static QStringList fontCatalogs() {
    QSettings st;
    st.beginGroup("fontCatalogs");
    QStringList names = st.childKeys();
    names.sort(Qt::CaseInsensitive);
    return names;
}

static QStringList catalogFonts(const QString &catalog) {
    QStringList fonts = QSettings().value("fontCatalogs/" + catalog).toStringList();
    fonts.removeAll(QString()); // an empty list may come back as one empty string
    return fonts;
}

static void setCatalogFonts(const QString &catalog, QStringList fonts) {
    fonts.removeDuplicates();
    fonts.sort(Qt::CaseInsensitive);
    QSettings().setValue("fontCatalogs/" + catalog, fonts);
}

QString StyleEditor::askCatalogName(QWidget *parent, const QString &old) {
    const QString name =
        QInputDialog::getText(parent, tr("Font catalog"), tr("Catalog name:"), QLineEdit::Normal, old).trimmed();
    if (name.isEmpty() || name == old) return {};
    // a slash would make a settings group of it
    if (name.contains('/') || name.contains('\\') || fontCatalogs().contains(name, Qt::CaseInsensitive)) {
        QMessageBox::warning(parent, tr("Font catalog"), tr("A catalog named \"%1\" cannot be made").arg(name));
        return {};
    }
    return name;
}

void StyleEditor::manageFontCatalogs(QWidget *parent, const QString &font) {
    QDialog d(parent);
    d.setWindowTitle(tr("Font catalogs"));
    auto *lay = new QHBoxLayout(&d);
    auto *left = new QVBoxLayout;
    auto *cats = new QListWidget(&d);
    left->addWidget(new QLabel(tr("Catalogs"), &d));
    left->addWidget(cats, 1);
    auto *buttons = new QHBoxLayout;
    auto *add = new QPushButton(tr("Add"), &d), *rename = new QPushButton(tr("Rename"), &d),
         *remove = new QPushButton(tr("Remove"), &d);
    for (auto *b : {add, rename, remove}) buttons->addWidget(b);
    left->addLayout(buttons);
    lay->addLayout(left, 1);
    auto *right = new QVBoxLayout;
    auto *filter = new QLineEdit(&d);
    filter->setPlaceholderText(tr("Filter fonts"));
    filter->setClearButtonEnabled(true);
    auto *onlyIn = new QCheckBox(tr("Only the fonts in the catalog"), &d);
    auto *fonts = new QListWidget(&d);
    fonts->setUniformItemSizes(true);
    right->addWidget(filter);
    right->addWidget(onlyIn);
    right->addWidget(fonts, 1);
    auto *close = new QDialogButtonBox(QDialogButtonBox::Close, &d);
    connect(close, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    right->addWidget(close);
    lay->addLayout(right, 2);
    const QStringList families = QFontDatabase::families();
    const auto current = [cats] { return cats->currentItem() ? cats->currentItem()->text() : QString(); };
    const auto fillFonts = [=] {
        const QString cat = current();
        const QStringList in = catalogFonts(cat);
        QSignalBlocker block(fonts);
        fonts->clear();
        // a catalog's fonts not installed here are listed too, to be taken out
        QStringList all = families;
        for (const QString &f : in)
            if (!all.contains(f)) all << f;
        for (const QString &f : all) {
            const bool member = in.contains(f);
            if (!f.contains(filter->text(), Qt::CaseInsensitive) || (onlyIn->isChecked() && !member)) continue;
            auto *it = new QListWidgetItem(f, fonts);
            it->setFont(QFont(f, fonts->font().pointSize()));
            it->setFlags(cat.isEmpty() ? Qt::NoItemFlags : Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
            it->setCheckState(member ? Qt::Checked : Qt::Unchecked);
        }
        if (auto found = fonts->findItems(font, Qt::MatchExactly); !found.isEmpty()) fonts->scrollToItem(found[0]);
    };
    const auto fillCats = [=](const QString &select) {
        cats->clear();
        cats->addItems(fontCatalogs());
        const auto found = cats->findItems(select, Qt::MatchExactly);
        cats->setCurrentRow(found.isEmpty() ? 0 : cats->row(found[0]));
        rename->setEnabled(cats->count());
        remove->setEnabled(cats->count());
        fillFonts();
    };
    connect(fonts, &QListWidget::itemChanged, &d, [=](QListWidgetItem *it) {
        QStringList in = catalogFonts(current());
        if (it->checkState() == Qt::Checked) in << it->text();
        else in.removeAll(it->text());
        setCatalogFonts(current(), in);
    });
    connect(cats, &QListWidget::currentRowChanged, &d, fillFonts);
    connect(filter, &QLineEdit::textChanged, &d, fillFonts);
    connect(onlyIn, &QCheckBox::toggled, &d, fillFonts);
    connect(add, &QPushButton::clicked, &d, [=, &d] {
        if (const QString name = askCatalogName(&d, {}); !name.isEmpty()) {
            setCatalogFonts(name, {});
            fillCats(name);
        }
    });
    connect(rename, &QPushButton::clicked, &d, [=, &d] {
        const QString old = current();
        if (const QString name = askCatalogName(&d, old); !name.isEmpty()) {
            setCatalogFonts(name, catalogFonts(old));
            QSettings().remove("fontCatalogs/" + old);
            fillCats(name);
        }
    });
    connect(remove, &QPushButton::clicked, &d, [=, &d] {
        if (QMessageBox::question(&d, tr("Font catalogs"), tr("Remove the catalog \"%1\"?").arg(current())) !=
            QMessageBox::Yes)
            return;
        QSettings().remove("fontCatalogs/" + current());
        fillCats({});
    });
    QString first;
    for (const QString &c : fontCatalogs())
        if (catalogFonts(c).contains(font)) {
            first = c;
            break;
        }
    fillCats(first);
    d.resize(640, 480);
    d.exec();
}

// Laid out like Kainote's style panel (two columns, colours with alpha under them, big preview);
// the Apply / rename / name-clash logic follows Aegisub's DialogStyleEditor.
StyleEditor::StyleEditor(const fsb::StyleData &style, QWidget *parent) : QDialog(parent), m_style(style) {
    setWindowTitle(tr("Style editor"));
    auto *v = new QVBoxLayout(this);
    auto *cols = new QHBoxLayout;
    auto *left = new QVBoxLayout, *right = new QVBoxLayout;
    cols->addLayout(left, 3);
    cols->addLayout(right, 2);
    v->addLayout(cols);
    auto group = [this](QBoxLayout *col, const QString &title, QLayout *content) {
        auto *g = new QGroupBox(title, this);
        g->setLayout(content);
        col->addWidget(g);
    };
    auto dspin = [this](double v, double lo, double hi) {
        auto *s = new QDoubleSpinBox(this);
        s->setRange(lo, hi);
        s->setDecimals(2);
        s->setValue(v);
        return s;
    };
    auto ispin = [this](int v) {
        auto *s = new QSpinBox(this);
        s->setRange(-9999, 99999); // Aegisub's ranges, here and below
        s->setValue(v);
        return s;
    };
    // a caption above each field, Kainote style
    auto labelled = [this](QGridLayout *g, int row, int col, const QString &text, QWidget *w) {
        g->addWidget(new QLabel(text, this), row * 2, col);
        g->addWidget(w, row * 2 + 1, col);
    };

    auto *nameL = new QHBoxLayout;
    m_name = new QLineEdit(fs::q(style.name), this);
    nameL->addWidget(m_name);
    group(left, tr("Style name"), nameL);

    auto *fontL = new QGridLayout;
    m_font = new QComboBox(this);
    m_font->setEditable(true);
    m_font->addItems(QFontDatabase::families());
    m_font->completer()->setCompletionMode(QCompleter::PopupCompletion);
    m_font->completer()->setFilterMode(Qt::MatchContains);
    m_font->setCurrentText(fs::q(style.font));
    if (!QFontDatabase::families().contains(m_font->currentText())) m_font->setToolTip(tr("Font is not installed"));
    m_size = dspin(style.size, 0, 10000);
    fontL->addWidget(m_font, 0, 0, 1, 3);
    fontL->addWidget(m_size, 0, 3);
    m_bold = new QCheckBox(tr("Bold"), this);
    m_italic = new QCheckBox(tr("Italic"), this);
    m_underline = new QCheckBox(tr("Underline"), this);
    m_strike = new QCheckBox(tr("Strikeout"), this);
    m_bold->setChecked(style.bold);
    m_italic->setChecked(style.italic);
    m_underline->setChecked(style.underline);
    m_strike->setChecked(style.strikeout);
    int fc = 0;
    for (auto *c : {m_bold, m_italic, m_underline, m_strike}) fontL->addWidget(c, 1, fc++);
    // Kainote's font catalogs: the font list narrowed to one, fonts added to them, and their window
    auto *catalog = new QComboBox(this);
    catalog->setToolTip(tr("Font catalog"));
    auto *charFilter = new QToolButton(this);
    charFilter->setText(tr("Filter"));
    charFilter->setCheckable(true);
    charFilter->setChecked(QSettings().value("style/fontFilterOn", false).toBool());
    charFilter->setToolTip(tr("Filters fonts to those containing the characters set in the settings"));
    const auto filterFonts = [this, catalog, charFilter] {
        const QString cat = catalog->currentData().toString(), text = m_font->currentText();
        QStringList families = QFontDatabase::families();
        if (cat == "/") { // the fonts in no catalog
            QSet<QString> listed;
            for (const QString &c : fontCatalogs())
                for (const QString &f : catalogFonts(c)) listed.insert(f);
            families.removeIf([&listed](const QString &f) { return listed.contains(f); });
        } else if (!cat.isEmpty()) {
            families = catalogFonts(cat);
        }
        if (const QString chars = QSettings().value("style/fontFilterText", kFontFilterChars).toString();
            charFilter->isChecked() && !chars.trimmed().isEmpty()) {
            QApplication::setOverrideCursor(Qt::WaitCursor);
            families.removeIf([&chars](const QString &f) { return !fontHasAll(f, chars); });
            QApplication::restoreOverrideCursor();
        }
        QSettings().setValue("style/fontFilterOn", charFilter->isChecked());
        const QSignalBlocker block(m_font);
        m_font->clear();
        m_font->addItems(families);
        m_font->setCurrentText(text);
        QSettings().setValue("style/fontCatalog", cat);
    };
    const auto fillCatalogs = [this, catalog, filterFonts] {
        const QSignalBlocker block(catalog);
        catalog->clear();
        catalog->addItem(tr("All fonts"), QString());
        catalog->addItem(tr("Fonts in no catalog"), QStringLiteral("/"));
        for (const QString &c : fontCatalogs()) catalog->addItem(c, c);
        catalog->setCurrentIndex(std::max(0, catalog->findData(QSettings().value("style/fontCatalog").toString())));
        filterFonts();
    };
    fillCatalogs();
    connect(catalog, &QComboBox::currentIndexChanged, this, filterFonts);
    connect(charFilter, &QToolButton::toggled, this, filterFonts);
    auto *addTo = new QToolButton(this);
    addTo->setText("+");
    addTo->setToolTip(tr("Add the font to a catalog"));
    connect(addTo, &QToolButton::clicked, this, [this, addTo, fillCatalogs] {
        const QString font = m_font->currentText().trimmed();
        if (font.isEmpty()) return;
        QMenu menu;
        for (const QString &c : fontCatalogs()) {
            auto *a = menu.addAction(fs::menuText(c), [c, font] {
                QStringList in = catalogFonts(c);
                if (in.contains(font)) in.removeAll(font);
                else in << font;
                setCatalogFonts(c, in);
            });
            a->setCheckable(true);
            a->setChecked(catalogFonts(c).contains(font));
        }
        menu.addSeparator();
        menu.addAction(tr("New catalog..."), [this, font] {
            if (const QString name = askCatalogName(this, {}); !name.isEmpty()) setCatalogFonts(name, {font});
        });
        menu.exec(addTo->mapToGlobal(QPoint(0, addTo->height())));
        fillCatalogs();
    });
    auto *manage = new QPushButton(tr("Catalogs..."), this);
    connect(manage, &QPushButton::clicked, this, [this, fillCatalogs] {
        manageFontCatalogs(this, m_font->currentText());
        fillCatalogs();
    });
    fontL->addWidget(catalog, 2, 0);
    fontL->addWidget(addTo, 2, 1);
    fontL->addWidget(manage, 2, 2);
    fontL->addWidget(charFilter, 2, 3);
    group(left, tr("Font and size"), fontL);

    auto *colorL = new QGridLayout;
    const char *names[] = {QT_TR_NOOP("Primary"), QT_TR_NOOP("Secondary"), QT_TR_NOOP("Outline"), QT_TR_NOOP("Shadow")};
    const rust::String *src[] = {&style.primary, &style.secondary, &style.outline_color, &style.back};
    for (int k = 0; k < 4; ++k) {
        m_colors[k] = fs::q(*src[k]);
        auto *b = m_colorButtons[k] = new QToolButton(this);
        b->setText(tr(names[k]));
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_alpha[k] = new QSpinBox(this);
        m_alpha[k]->setRange(0, 255);
        m_alpha[k]->setValue(255 - assColor(m_colors[k]).alpha());
        m_alpha[k]->setToolTip(tr("Transparency (0 = opaque)"));
        auto paint = [this, k] {
            QColor c = assColor(m_colors[k]);
            c.setAlpha(255);
            m_colorButtons[k]->setStyleSheet(QString("QToolButton{background:%1;color:%2;border:1px solid palette(mid);padding:3px}")
                                                 .arg(c.name(), c.lightness() > 140 ? "black" : "white"));
        };
        paint();
        // the transparency picked too, and the preview follows the picker; Cancel puts it back (Aegisub)
        connect(b, &QToolButton::clicked, this, [this, k, paint] {
            const QString before = m_colors[k];
            const int alphaBefore = m_alpha[k]->value();
            QColor start = assColor(m_colors[k]);
            start.setAlpha(255 - alphaBefore);
            const QColor taken = ColorPicker::get(this, start, true, [&](const QColor &c) {
                m_colors[k] = assColorString(c);
                {
                    const QSignalBlocker block(m_alpha[k]);
                    m_alpha[k]->setValue(255 - c.alpha());
                }
                paint();
                preview();
            });
            if (taken.isValid()) return;
            m_colors[k] = before;
            m_alpha[k]->setValue(alphaBefore);
            paint();
            preview();
        });
        colorL->addWidget(b, 0, k);
        colorL->addWidget(m_alpha[k], 1, k);
    }
    group(left, tr("Colors and transparency"), colorL);

    auto *prevL = new QVBoxLayout;
    m_previewText = new QLineEdit(QSettings().value("styles/previewText", tr("Sample text 0123 ĄĘŻŹ")).toString(), this);
    m_preview = new QLabel(this);
    m_preview->setMinimumSize(420, 110);
    m_preview->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored); // the pixmap must not dictate the size
    // the preview's background, a colour or the checkerboard (Aegisub's colour button)
    m_previewBg = QSettings().value("styles/previewBg").value<QColor>();
    auto *bg = new QToolButton(this);
    bg->setIcon(icon("palette"));
    bg->setToolTip(tr("Preview background"));
    bg->setPopupMode(QToolButton::InstantPopup);
    auto *bgMenu = new QMenu(bg);
    const auto setBg = [this](QColor c) {
        m_previewBg = c;
        QSettings().setValue("styles/previewBg", c);
        preview();
    };
    bgMenu->addAction(tr("Checkerboard"), this, [setBg] { setBg({}); });
    bgMenu->addAction(tr("Colour..."), this, [this, setBg] {
        if (const QColor c = ColorPicker::get(this, m_previewBg.isValid() ? m_previewBg : Qt::black, false); c.isValid())
            setBg(c);
    });
    bg->setMenu(bgMenu);
    auto *textRow = new QHBoxLayout;
    textRow->addWidget(m_previewText, 1);
    textRow->addWidget(bg);
    prevL->addLayout(textRow);
    prevL->addWidget(m_preview, 1);
    group(left, tr("Preview"), prevL);
    connect(m_previewText, &QLineEdit::textChanged, this, &StyleEditor::preview);

    auto *borderL = new QGridLayout;
    m_outline = dspin(style.outline, 0, 1000);
    m_shadow = dspin(style.shadow, 0, 1000);
    m_border = new QComboBox(this); // border style 1, 3 or 4, as Aegisub offers them
    m_border->addItems({tr("Outline"), tr("Opaque box"), tr("Shadow box (libass)")});
    m_border->setCurrentIndex(style.border_style == 3 ? 1 : style.border_style == 4 ? 2 : 0);
    labelled(borderL, 0, 0, tr("Outline:"), m_outline);
    labelled(borderL, 0, 1, tr("Shadow:"), m_shadow);
    borderL->addWidget(m_border, 2, 0, 1, 2);
    group(right, tr("Outline"), borderL);

    auto *geoL = new QGridLayout;
    m_scaleX = dspin(style.scale_x, 0, 10000);
    m_scaleY = dspin(style.scale_y, 0, 10000);
    m_angle = dspin(style.angle, -360, 360);
    m_spacing = dspin(style.spacing, -1000, 1000);
    labelled(geoL, 0, 0, tr("Scale X:"), m_scaleX);
    labelled(geoL, 0, 1, tr("Scale Y:"), m_scaleY);
    labelled(geoL, 1, 0, tr("Angle:"), m_angle);
    labelled(geoL, 1, 1, tr("Spacing:"), m_spacing);
    group(right, tr("Miscellaneous"), geoL);

    auto *marginL = new QGridLayout;
    m_ml = ispin(style.margin_l);
    m_mr = ispin(style.margin_r);
    m_mv = ispin(style.margin_v);
    labelled(marginL, 0, 0, tr("Left:"), m_ml);
    labelled(marginL, 0, 1, tr("Right:"), m_mr);
    labelled(marginL, 0, 2, tr("Vertical:"), m_mv);
    group(right, tr("Margins"), marginL);

    auto *alignL = new QGridLayout;
    auto *ag = new QButtonGroup(this);
    for (int k = 0; k < 9; ++k) {
        m_align[k] = new QRadioButton(QString::number(k + 1), this);
        ag->addButton(m_align[k]);
        alignL->addWidget(m_align[k], 2 - k / 3, k % 3);
    }
    m_align[std::clamp(style.alignment, 1, 9) - 1]->setChecked(true);
    connect(ag, &QButtonGroup::buttonToggled, this, &StyleEditor::preview);
    group(right, tr("Alignment"), alignL);

    // Aegisub's encoding list; the number before " - " is what gets stored
    auto *encL = new QHBoxLayout;
    m_encodingBox = new QComboBox(this);
    m_encodingBox->setEditable(true);
    m_encodingBox->addItems({"0 - ANSI", "1 - Default", "2 - Symbol", "77 - Mac", "128 - Shift_JIS", "129 - Hangeul",
                             "130 - Johab", "134 - GB2312", "136 - Chinese BIG5", "161 - Greek", "162 - Turkish",
                             "163 - Vietnamese", "177 - Hebrew", "178 - Arabic", "186 - Baltic", "204 - Russian",
                             "222 - Thai", "238 - East European", "255 - OEM"});
    const int enc = m_encodingBox->findText(QString::number(style.encoding) + " - ", Qt::MatchStartsWith);
    if (enc >= 0) m_encodingBox->setCurrentIndex(enc);
    else m_encodingBox->setCurrentText(QString::number(style.encoding));
    encL->addWidget(m_encodingBox);
    group(right, tr("Encoding"), encL);
    right->addStretch();

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(bb->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] {
        if (onApply) onApply(this->style());
    });
    v->addWidget(bb);
    for (auto *w : findChildren<QDoubleSpinBox *>()) connect(w, &QDoubleSpinBox::valueChanged, this, &StyleEditor::preview);
    for (auto *w : findChildren<QSpinBox *>()) connect(w, &QSpinBox::valueChanged, this, &StyleEditor::preview);
    for (auto *w : findChildren<QCheckBox *>()) connect(w, &QCheckBox::toggled, this, &StyleEditor::preview);
    connect(m_font, &QComboBox::currentTextChanged, this, &StyleEditor::preview);
    connect(m_border, &QComboBox::currentIndexChanged, this, &StyleEditor::preview);
    resize(900, sizeHint().height());
    preview();
}

void StyleEditor::accept() {
    if (onApply && !onApply(style())) return;
    QSettings().setValue("styles/previewText", m_previewText->text());
    QDialog::accept();
}

void StyleEditor::resizeEvent(QResizeEvent *e) {
    QDialog::resizeEvent(e);
    QTimer::singleShot(0, this, &StyleEditor::preview); // once the layout gave the preview its new size
}

fsb::StyleData StyleEditor::style() const {
    fsb::StyleData s = m_style;
    s.name = fs::rs(m_name->text().replace(',', ';'));
    s.font = fs::rs(m_font->currentText().replace(',', ';')); // a comma would split the Style line
    s.size = m_size->value();
    rust::String *dst[] = {&s.primary, &s.secondary, &s.outline_color, &s.back};
    for (int k = 0; k < 4; ++k) {
        QColor c = assColor(m_colors[k]);
        c.setAlpha(255 - m_alpha[k]->value());
        *dst[k] = fs::rs(assColorString(c));
    }
    s.bold = m_bold->isChecked();
    s.italic = m_italic->isChecked();
    s.underline = m_underline->isChecked();
    s.strikeout = m_strike->isChecked();
    s.scale_x = m_scaleX->value();
    s.scale_y = m_scaleY->value();
    s.spacing = m_spacing->value();
    s.angle = m_angle->value();
    s.border_style = std::array{1, 3, 4}[size_t(std::max(0, m_border->currentIndex()))];
    s.outline = m_outline->value();
    s.shadow = m_shadow->value();
    for (int k = 0; k < 9; ++k)
        if (m_align[k]->isChecked()) s.alignment = k + 1;
    s.margin_l = m_ml->value();
    s.margin_r = m_mr->value();
    s.margin_v = m_mv->value();
    s.encoding = m_encodingBox->currentText().section(' ', 0, 0).toInt();
    return s;
}

// Render the preview text with libass on a checkerboard.
void StyleEditor::preview() {
    if (!m_preview) return;
    const int w = std::max(420, m_preview->width()), h = std::max(110, m_preview->height());
    QImage img(w, h, QImage::Format_RGBA8888);
    if (m_previewBg.isValid()) img.fill(m_previewBg);
    else {
        QPainter p(&img);
        for (int y = 0; y < h; y += 10)
            for (int x = 0; x < w; x += 10)
                p.fillRect(x, y, 10, 10, ((x + y) / 10) % 2 ? QColor(0x99, 0x99, 0x99) : QColor(0x66, 0x66, 0x66));
    }
    auto st = style();
    st.name = "Preview";
    const QString script =
        QString("[Script Info]\nScriptType: v4.00+\nPlayResX: %1\nPlayResY: %2\n\n[V4+ Styles]\nFormat: Name, Fontname, "
                "Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, "
                "StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, "
                "MarginR, MarginV, Encoding\n%3\n\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, "
                "MarginR, MarginV, Effect, Text\nDialogue: 0,0:00:00.00,0:01:00.00,Preview,,0,0,0,,{\\an5\\q2}%4\n")
            .arg(w).arg(h).arg(fs::q(fsb::style_lines({&st, 1})), m_previewText->text());
    try {
        static std::optional<rust::Box<fsb::Renderer>> r;
        if (!r) r.emplace(fsb::new_renderer());
        (*r)->set_script(fs::Utf8(script));
        (*r)->blend(1000, w, h, w, h, {img.bits(), size_t(img.sizeInBytes())});
    } catch (const rust::Error &) {
        // no preview without libass; the editor still works
    }
    m_preview->setPixmap(QPixmap::fromImage(img));
}
