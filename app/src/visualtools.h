// Visual typesetting tools on the video (Kainote's tool row): position
// pointer, \pos, \move, scale, Z and X/Y rotation, rectangle and vector
// \clip / \iclip, \p drawings, the position shifter and the all-tags tool.
#pragma once

#include "bridge.h"
#include "hydra.h"

#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QTransform>
#include <array>
#include <future>
#include <optional>
#include <set>
#include <span>
#include <vector>

class VideoBox;
class QPainter;
class QMouseEvent;
class QKeyEvent;
class QWheelEvent;
class QWidget;
class QStackedWidget;
class QToolButton;
class QComboBox;
class QDoubleSpinBox;

// A parsed ASS drawing: one command letter with its points each.
struct Drawing {
    struct Cmd {
        char type; // m n l b s p c
        std::vector<QPointF> pts;
    };
    std::vector<Cmd> cmds;
    static Drawing parse(const QString &s);
    QString toString() const;
    // flat point list (cmd, index) for hit testing
    std::vector<std::pair<int, int>> handles() const;
    QPointF &at(std::pair<int, int> h) { return cmds[h.first].pts[h.second]; }
    QRectF bounds() const;
    void translate(QPointF d);
    QPointF last() const; // the last point of the figure being drawn
};

// A tool of the row: its command id (the Video menu, the hotkey editor), icon and name (translated
// in the VisualTools context)
struct ToolInfo {
    const char *id, *icon, *name;
};

class VisualTools : public QObject {
    Q_OBJECT
public:
    static std::span<const ToolInfo> tools(); // in Tool order
    bool shown() const; // the active line is on the video frame
    enum Tool { Pointer, Pos, Move, Scale, RotZ, RotXY, RClip, VClip, Draw, Shifter, AllTags, ToolCount };
    enum VecMode { VecMove, VecLine, VecBezier, VecNewFigure, VecDelete, VecFreehand, VecInsert, VecConvert, VecSpline, VecModeCount };
    // the vector clip and drawing modes, in VecMode order (Aegisub's video/tool/vclip/*)
    static constexpr const char *kVecModes[VecModeCount] = {
        QT_TR_NOOP("Move points"), QT_TR_NOOP("Add line"), QT_TR_NOOP("Add Bézier curve"),
        QT_TR_NOOP("Add separate point (move)"), QT_TR_NOOP("Delete point"), QT_TR_NOOP("Draw freehand"),
        QT_TR_NOOP("Insert a point into a segment"), QT_TR_NOOP("Convert a segment between line and Bézier"),
        QT_TR_NOOP("Add B-spline point")};
    void setVecMode(int k); // a vector mode; it takes the vector clip tool unless a vector tool is on

    explicit VisualTools(VideoBox *box);
    int tool() const { return m_tool; }
    void setTool(int t);
    QWidget *toolRow(QWidget *parent); // tool buttons
    QWidget *options() const; // the tool's own options, a row of its own (made by toolRow)

    void reload(); // active line changed or edited
    void paint(QPainter &p);
    bool press(QMouseEvent *e);
    bool move(QMouseEvent *e);
    bool release(QMouseEvent *e);
    bool doubleClick(QMouseEvent *e);
    bool key(QKeyEvent *e);
    bool wheel(QWheelEvent *e); // the all-tags tool's sliders and tags (Kainote)

private:
    QPointF w(QPointF s) const; // script -> widget
    QPointF s(QPointF w) const; // widget -> script
    // allSelected: scale, rotation and clips go to every selected line (Aegisub SetSelectedOverride)
    void apply(const QString &tags, bool allSelected = false);
    void applyPos(QPointF p);
    void toggleMove();
    // the all-tags tool (Kainote's Hydra): the sliders m_hv to the lines grabbed, from their texts
    // in m_tagBase and the active line's values m_hFrom
    void applyHydra();
    void hydraValues(); // the sliders from the active line
    const hydra::Tag &htag() const; // the tag picked
    QLineF sliderLine(int i) const;       // slider i on the widget
    int sliderAt(QPointF at) const;       // the slider under a widget point, or -1
    double snapped(int i, double v) const; // to the tag's steps and digits, in slider i's range
    // the selected lines and their texts, m_tagRows and m_tagBase, as a drag starts; and the edit
    // box's selection, m_tagSel
    void grabTexts();
    // Kainote's "change all tags": every occurrence of the comma-separated tags, from m_tagBase
    // (mode as Session::adjust_tag: 0 add, 7 scale)
    void adjustAll(const char *tags, uint8_t mode, std::initializer_list<double> deltas);
    // Kainote's "preserve proportions": the lines' places scaled and turned around `around`
    void keepPlaces(QPointF around, double sx, double sy, double turn);
    void writeTrack(); // the move tool's \move from its two points and their video times
    // Kainote's rectangles (the clip's, and the position and scale tools'): a corner (10-13), an
    // edge (20-23, x1 x2 y1 y2) or inside (30) of r (script coordinates) under the widget point,
    // else 1, a new one
    int rectHit(QRectF r, QPointF at) const;
    // r as the drag `code` changes it: by `by`, or (1) the new one from `from` to `to`
    static QRectF rectDrag(QRectF r, int code, QPointF by, QPointF from, QPointF to);
    void paintRect(QPainter &p, QRectF r, QColor c) const;
    bool measure();  // m_fitBox: the active line as libass draws it now; false when nothing shows
    void prepareMeasurer(); // starts reading the video's fonts for measure(), off the GUI thread
    void fitPos();   // the grabbed lines moved so the active one sits in m_fit by m_posAlign
    void fitScale(); // the grabbed lines' scale tags changed so the active line fills m_fit
    QString clipTag() const;
    void writeRectClip(); // m_v's rectangle to the grabbed lines, each keeping its own \iclip (Aegisub)
    void writeVector();
    // how far libass moves \p drawing points for the line's alignment
    QPointF alignShift(const Drawing &d) const;
    int hitHandle(QPointF widgetPos) const;
    QPointF vecW(QPointF pt) const; // vector point -> widget
    // drawing point (offset from \pos, unscaled) <-> script coordinates, turned by \frz around \org
    QPointF drawToScript(QPointF pt) const;
    QPointF scriptToDraw(QPointF s) const;
    // libass's 3D transform of a line (Session's line_projection) for this video, shearing from
    // `at`; and a point (x, y, z) as laid out to where it shows, script pixels both
    using Projection = std::array<double, 12>;
    Projection projection(const fsb::Visual &v, QPointF at) const;
    static QPointF project(const Projection &m, QPointF p, double z = 0);
    // the drawing's frame: its points' origin and scale, and its (0,0) where libass puts it
    void setDrawFrame(QPointF origin, QPointF scale, QPointF at);
    double widgetScale() const; // widget pixels a script pixel
    // a guide `px` widget pixels long in script pixels, at most half the camera's distance so
    // libass's strong perspective does not blow it up
    double guide(double px) const;
    // the l/b command whose segment passes near the point (t: where on it), or -1
    int hitSegment(QPointF widgetPos, double &t) const;
    // the active line is on the video's frame and not a comment: only then the tools work (Aegisub)
    bool vecKey(QKeyEvent *e); // Ctrl+A, Delete and WASD on the selected vector points
    QPointF toVec(QPointF script) const; // script coordinates -> the vector's own units
    void grabLines();         // the selected lines, as they are, for moveLines
    void moveLines(QPointF d); // the grabbed lines moved by d

    VideoBox *m_box;
    int m_tool = Pointer;
    int m_vecMode = VecMove;
    class QButtonGroup *m_vecModes = nullptr;
    fsb::Visual m_v{};
    bool m_valid = false;
    qint64 m_first = 0, m_last = -1; // the frames the active line is shown on
    std::vector<size_t> m_rows;      // the lines a drag moves, and how they were
    std::vector<fsb::Visual> m_starts;
    // the other selected lines on the frame and where they are on the widget (the position tool)
    std::vector<std::pair<size_t, QPointF>> others() const;
    Drawing m_vec;            // vector clip or drawing being edited
    QPointF m_drawOrigin, m_drawScale{1, 1};
    Projection m_drawProj{};
    QTransform m_drawUnproject; // the shown plane back to the laid-out one
    // drag state
    int m_drag = 0;           // 0 none, else tool-specific handle id
    Qt::MouseButton m_button = Qt::NoButton;
    QPointF m_pressS, m_pressW, m_mouseW;
    fsb::Visual m_start{};
    std::pair<int, int> m_vecHandle{-1, -1};
    // the vector points selected (Aegisub: a box or Ctrl+click), of line m_vecRow, and where they
    // were when a drag began
    std::set<std::pair<int, int>> m_vecSel;
    int m_vecRow = -1;
    std::vector<std::pair<std::pair<int, int>, QPointF>> m_vecStart;
    int m_dragId = 0;
    QString m_amend;
    QStackedWidget *m_sub = nullptr;
    int m_shiftWhat = 47; // all but drawing points
    // scale tool: 1 width, 2 keep proportions, 4 height, 8 change all scale tags, 16 preserve
    // proportions (the lines' places scaled too)
    int m_scaleWhat = 5;
    // rotation tools: 1 change all \frz, 2 preserve proportions (the places turned too), 4 change
    // all \frx and \fry
    int m_rotWhat = 0;
    // position tool: 1 position by a rectangle (Kainote), 2 on its X axis, 4 on its Y axis; the scale
    // tool adds 32 its own rectangle for the text as it is, 64 scale by a rectangle
    int m_posWhat = 6;
    QComboBox *m_posAlign = nullptr;
    QWidget *m_posOptions = nullptr;
    // the rectangle the position and scale tools fit the line to (red), the scale tool's for the
    // text as it is (blue), in script coordinates, and where they were as a drag began
    QRectF m_fit, m_fitStart, m_orig, m_origStart;
    bool m_fitOn = false, m_origOn = false;
    std::vector<double> m_fitBox; // Renderer::line_box of the active line as a drag began
    std::optional<rust::Box<fsb::Renderer>> m_measurer; // with the fonts of m_measurerVideo
    std::future<rust::Box<fsb::Renderer>> m_measurerNext; // m_measurer, while its fonts are read
    QString m_measurerVideo;
    QComboBox *m_shapeBox = nullptr; // ready shapes dragged out on the video (Kainote)
    size_t m_shapeFrom = 0;          // first command of the shape being dragged
    QPointF m_shapeA;                // its press point in drawing units
    bool m_twoPoints = false; // Z rotation: the angle of a line dragged on the video
    // move tool (Kainote): the movement of something in the video from two points at two times
    bool m_track = false;
    QWidget *m_trackButton = nullptr;
    int m_trackPts = 0, m_trackRow = -1;
    QPointF m_trackP[2];
    qint64 m_trackT[2] = {0, 0};
    QWidget *m_invert = nullptr;
    QComboBox *m_tagName = nullptr, *m_tagMode = nullptr;
    QDoubleSpinBox *m_tagValues[4] = {};
    std::vector<size_t> m_tagRows;
    rust::Vec<rust::String> m_tagBase; // the rows' texts when the drag started
    std::pair<qint64, qint64> m_tagSel{-1, -1}; // the edit box's selection then, bytes
    std::vector<hydra::Tag> m_hydra;
    // the sliders: their values, the active line's when the change began, and their ranges (the
    // tag's, taking in the line's value)
    double m_hv[4] = {}, m_hFrom[4] = {}, m_hMin[4] = {}, m_hMax[4] = {};
    double m_sliderY = 40, m_sliderYStart = 0; // the first slider's height over the video
    std::vector<QToolButton *> m_buttons;
};
