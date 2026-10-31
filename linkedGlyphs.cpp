#include "linkedGlyphs.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <algorithm>
#include <climits>
#include <map>
#include <optional>
#include <tuple>

#include "Unicode/UnicodeNames.hpp"

using LinkedGlyphs::PixelSet;

namespace {

auto toPixels(const Bitmap &bitmap, const GlyphInfo &info) -> PixelSet {
  PixelSet pixels;
  for (int row = 0; row < bitmap.dim.height; row++) {
    for (int col = 0; col < bitmap.dim.width; col++) {
      size_t idx = static_cast<size_t>(row) * bitmap.dim.width + col;
      if ((idx < bitmap.pixels.size()) && (bitmap.pixels[idx] != 0)) {
        pixels.insert({col - info.horizontalOffset, row - info.verticalOffset});
      }
    }
  }
  return pixels;
}

auto shifted(const PixelSet &pixels, int dx) -> PixelSet {
  PixelSet result;
  for (auto &p : pixels) result.insert({p.first + dx, p.second});
  return result;
}

auto containsAll(const PixelSet &big, const PixelSet &small) -> bool {
  return std::all_of(small.begin(), small.end(), [&](auto &p) { return big.count(p) != 0; });
}

auto containsNone(const PixelSet &big, const PixelSet &small) -> bool {
  return std::none_of(small.begin(), small.end(), [&](auto &p) { return big.count(p) != 0; });
}

auto overlap(const PixelSet &a, const PixelSet &b) -> int {
  int count = 0;
  for (auto &p : b) count += a.count(p);
  return count;
}

auto difference(const PixelSet &a, const PixelSet &b) -> PixelSet {
  PixelSet result;
  std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::inserter(result, result.end()));
  return result;
}

auto intersection(const PixelSet &a, const PixelSet &b) -> PixelSet {
  PixelSet result;
  std::set_intersection(a.begin(), a.end(), b.begin(), b.end(),
                        std::inserter(result, result.end()));
  return result;
}

auto inkWidth(const PixelSet &pixels) -> int {
  if (pixels.empty()) return 0;
  auto [minIt, maxIt] = std::minmax_element(pixels.begin(), pixels.end(),
                                            [](auto &a, auto &b) { return a.first < b.first; });
  return maxIt->first - minIt->first + 1;
}

// Builds a bitmap and metrics from pixels, keeping the non-geometric fields of templateInfo.
auto fromPixels(const PixelSet &pixels, const GlyphInfo &templateInfo, BitmapPtr &bitmap,
                GlyphInfoPtr &info) -> bool {
  info   = std::make_shared<GlyphInfo>(templateInfo);
  bitmap = std::make_shared<Bitmap>();
  if (pixels.empty()) {
    info->bitmapWidth  = 0;
    info->bitmapHeight = 0;
    return true;
  }
  int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;
  for (auto &p : pixels) {
    minX = std::min(minX, p.first);
    maxX = std::max(maxX, p.first);
    minY = std::min(minY, p.second);
    maxY = std::max(maxY, p.second);
  }
  int width = maxX - minX + 1, height = maxY - minY + 1;
  if ((width > 255) || (height > 255) || (-minX < INT8_MIN) || (-minX > INT8_MAX) ||
      (-minY < INT8_MIN) || (-minY > INT8_MAX)) {
    return false;
  }
  bitmap->dim = Dim(width, height);
  bitmap->pixels.assign(static_cast<size_t>(width) * height, 0);
  for (auto &p : pixels) {
    bitmap->pixels[static_cast<size_t>(p.second - minY) * width + (p.first - minX)] = 0xFF;
  }
  info->bitmapWidth      = width;
  info->bitmapHeight     = height;
  info->horizontalOffset = -minX;
  info->verticalOffset   = -minY;
  return true;
}

auto isValidGlyphCode(GlyphCode code) -> bool {
  return (code != NO_GLYPH_CODE) && (code != SPACE_CODE);
}

auto linkedGlyphCodes(IBMFFontModPtr font, int faceIdx)
    -> std::map<GlyphCode, std::vector<GlyphCode>> {
  std::map<GlyphCode, std::vector<GlyphCode>> links;
  int count = font->getFaceHeader(faceIdx)->glyphCount;
  for (int g = 0; g < count; g++) {
    GlyphInfoPtr    info;
    BitmapPtr       bitmap;
    GlyphLigKernPtr lk;
    if (font->getGlyph(faceIdx, g, info, bitmap, lk) && (info->mainCode != g) &&
        (info->mainCode < count)) {
      links[info->mainCode].push_back(g);
    }
  }
  return links;
}

auto findReferenceFace(IBMFFontModPtr font, int faceIdx, IBMFFontModPtr reference) -> int {
  uint8_t pointSize = font->getFaceHeader(faceIdx)->pointSize;
  for (int i = 0; i < reference->getPreamble().faceCount; i++) {
    if (reference->getFaceHeader(i)->pointSize == pointSize) return i;
  }
  return -1;
}

constexpr int SHIFTS[] = {0, -1, 1, -2, 2, -3, 3};

auto proposeFor(IBMFFontModPtr font, int faceIdx, GlyphCode baseGlyphCode,
                const std::vector<GlyphCode> &linked, const Bitmap &oldBase,
                const GlyphInfo &oldBaseInfo, const Bitmap &newBase,
                const GlyphInfo &newBaseInfo, IBMFFontModPtr reference, int referenceFaceIdx)
    -> std::vector<LinkedGlyphProposal> {
  std::vector<LinkedGlyphProposal> proposals;

  PixelSet oldPixels    = toPixels(oldBase, oldBaseInfo);
  PixelSet newPixels    = toPixels(newBase, newBaseInfo);
  FIX16    advanceDelta = newBaseInfo.advance - oldBaseInfo.advance;
  PixelSet added        = difference(newPixels, oldPixels);
  PixelSet removed      = difference(oldPixels, newPixels);
  if (added.empty() && removed.empty() && (advanceDelta == 0)) return proposals;

  for (GlyphCode g : linked) {
    GlyphInfoPtr    info;
    BitmapPtr       bitmap;
    GlyphLigKernPtr lk;
    if (!font->getGlyph(faceIdx, g, info, bitmap, lk)) continue;
    PixelSet current = toPixels(*bitmap, *info);

    // Find where the old base sits inside the linked glyph, preferring no shift.
    int  dx    = 0;
    bool found = false;
    for (int candidate : SHIFTS) {
      if (containsAll(current, shifted(oldPixels, candidate))) {
        dx    = candidate;
        found = true;
        break;
      }
    }

    if (!found) {
      // Skip glyphs that already carry the new base shape (e.g. edited by hand).
      bool alreadyApplied = false;
      for (int candidate : SHIFTS) {
        if (containsAll(current, shifted(newPixels, candidate)) &&
            containsNone(current, shifted(removed, candidate))) {
          alreadyApplied = true;
          break;
        }
      }
      if (alreadyApplied) continue;

      int best = -1;
      for (int candidate : SHIFTS) {
        int score = overlap(current, shifted(oldPixels, candidate));
        if (score > best) {
          best = score;
          dx   = candidate;
        }
      }
    }

    LinkedGlyphProposal proposal;
    proposal.added        = shifted(added, dx);
    proposal.removed      = shifted(removed, dx);
    proposal.expectedBase = shifted(oldPixels, dx);
    proposal.before       = current;
    proposal.computed     = difference(current, proposal.removed);
    proposal.computed.insert(proposal.added.begin(), proposal.added.end());
    proposal.after = proposal.computed;
    if ((proposal.after == current) && (advanceDelta == 0)) continue;

    proposal.faceIdx              = faceIdx;
    proposal.glyphCode            = g;
    proposal.codePoint            = font->getUTF32(g);
    proposal.baseCodePoint        = font->getUTF32(baseGlyphCode);
    proposal.oldInfo              = info;
    proposal.oldBitmap            = bitmap;
    proposal.ligKern              = lk;
    proposal.templateInfo         = *info;
    proposal.templateInfo.advance = info->advance + advanceDelta;
    proposal.recommended          = found;

    if (!found) {
      proposal.note = "Old base shape not found intact (orange outlines); check carefully";
    } else if (dx != 0) {
      proposal.note = QString("Base found shifted %1 px").arg(dx);
    }
    if (found && (inkWidth(oldPixels) != inkWidth(newPixels))) {
      proposal.note += QString(proposal.note.isEmpty() ? "" : "; ") +
                       "Base width changed; check accent centering";
    }

    if ((reference != nullptr) && (referenceFaceIdx >= 0)) {
      GlyphCode refCode = reference->translate(proposal.codePoint);
      if (isValidGlyphCode(refCode)) {
        GlyphInfoPtr    refInfo;
        BitmapPtr       refBitmap;
        GlyphLigKernPtr refLk;
        if (reference->getGlyph(referenceFaceIdx, refCode, refInfo, refBitmap, refLk) &&
            ((toPixels(*refBitmap, *refInfo) != current) || (refInfo->advance != info->advance))) {
          proposal.recommended = false;
          proposal.note        = "Already edited since the reference; check carefully";
        }
      }
    }

    proposals.push_back(proposal);
  }
  return proposals;
}

auto codePointString(char32_t codePoint) -> QString { return QString::fromUcs4(&codePoint, 1); }

auto describe(char32_t codePoint) -> QString {
  QString name = unicodeName(codePoint);
  return name.isEmpty() ? QString("(unnamed)") : name;
}

// Before/after preview of a proposal. Clicking or dragging on the after panel toggles pixels.
class ComparisonWidget : public QWidget {
public:
  ComparisonWidget(LinkedGlyphProposal *proposal, std::function<void()> onEdit, QWidget *parent)
      : QWidget(parent), proposal_(proposal), onEdit_(std::move(onEdit)) {
    PixelSet all = proposal_->before;
    for (auto *set : {&proposal_->after, &proposal_->expectedBase, &proposal_->added,
                      &proposal_->removed}) {
      all.insert(set->begin(), set->end());
    }
    if (all.empty()) all.insert({0, 0});
    minX_ = minY_ = INT_MAX;
    maxX_ = maxY_ = INT_MIN;
    for (auto &p : all) {
      minX_ = std::min(minX_, p.first);
      maxX_ = std::max(maxX_, p.first);
      minY_ = std::min(minY_, p.second);
      maxY_ = std::max(maxY_, p.second);
    }
    minX_ -= MARGIN;
    maxX_ += MARGIN;
    minY_ -= MARGIN;
    maxY_ += MARGIN;
    missingBase_    = difference(proposal_->expectedBase, proposal_->before);
    addConflicts_   = intersection(proposal_->added, proposal_->before);
    removeConflicts_ = difference(proposal_->removed, proposal_->before);
    setFixedSize((panelWidth() * 2 + GAP) * SCALE, panelHeight() * SCALE);
    setCursor(Qt::PointingHandCursor);
    setToolTip("Click or drag on the right-hand preview to toggle pixels");
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.fillRect(rect(), Qt::white);
    for (int y = minY_; y <= maxY_; y++) {
      for (int x = minX_; x <= maxX_; x++) {
        auto p      = std::make_pair(x, y);
        bool wasOn  = proposal_->before.count(p) != 0;
        bool isOn   = proposal_->after.count(p) != 0;
        bool compOn = proposal_->computed.count(p) != 0;

        drawCell(painter, 0, p, wasOn ? QColor(Qt::black) : QColor(Qt::white),
                 missingBase_.count(p) != 0);

        QColor color;
        if (isOn != compOn) {
          color = isOn ? QColor(0, 150, 60) : QColor(190, 235, 190);
        } else if (isOn && !wasOn) {
          color = QColor(0, 110, 255);
        } else if (!isOn && wasOn) {
          color = QColor(255, 175, 175);
        } else {
          color = isOn ? QColor(Qt::black) : QColor(Qt::white);
        }
        drawCell(painter, 1, p, color, addConflicts_.count(p) || removeConflicts_.count(p));
      }
    }

    painter.setPen(QPen(QColor(120, 120, 120), 2));
    int midY   = height() / 2;
    int arrowL = panelWidth() * SCALE + SCALE / 2;
    int arrowR = (panelWidth() + GAP) * SCALE - SCALE / 2;
    painter.drawLine(arrowL, midY, arrowR, midY);
    painter.drawLine(arrowR, midY, arrowR - SCALE, midY - SCALE);
    painter.drawLine(arrowR, midY, arrowR - SCALE, midY + SCALE);
  }

  void mousePressEvent(QMouseEvent *event) override {
    auto p = cellAt(event->position().toPoint());
    if (!p) return;
    paintValue_ = proposal_->after.count(*p) == 0;
    setPixel(*p);
  }

  void mouseMoveEvent(QMouseEvent *event) override {
    if (!(event->buttons() & Qt::LeftButton)) return;
    if (auto p = cellAt(event->position().toPoint())) setPixel(*p);
  }

private:
  static constexpr int SCALE  = 8;
  static constexpr int GAP    = 3;
  static constexpr int MARGIN = 2;

  auto panelWidth() const -> int { return maxX_ - minX_ + 1; }
  auto panelHeight() const -> int { return maxY_ - minY_ + 1; }

  void drawCell(QPainter &painter, int panel, std::pair<int, int> p, QColor color,
                bool conflict) {
    int   originX = panel * (panelWidth() + GAP);
    QRect cell((originX + p.first - minX_) * SCALE, (p.second - minY_) * SCALE, SCALE, SCALE);
    painter.fillRect(cell, color);
    painter.setPen(QColor(232, 232, 232));
    painter.drawRect(cell.adjusted(0, 0, -1, -1));
    if (conflict) {
      painter.setPen(QPen(QColor(255, 140, 0), 2));
      painter.drawRect(cell.adjusted(1, 1, -1, -1));
    }
  }

  auto cellAt(QPoint pos) const -> std::optional<std::pair<int, int>> {
    int col = pos.x() / SCALE - (panelWidth() + GAP);
    int row = pos.y() / SCALE;
    if ((pos.x() < 0) || (col < 0) || (col >= panelWidth()) || (row < 0) ||
        (row >= panelHeight())) {
      return std::nullopt;
    }
    return std::make_pair(col + minX_, row + minY_);
  }

  void setPixel(std::pair<int, int> p) {
    bool isOn = proposal_->after.count(p) != 0;
    if (isOn == paintValue_) return;
    if (paintValue_) {
      proposal_->after.insert(p);
    } else {
      proposal_->after.erase(p);
    }
    update();
    if (onEdit_) onEdit_();
  }

  LinkedGlyphProposal  *proposal_;
  std::function<void()> onEdit_;
  int                   minX_, maxX_, minY_, maxY_;
  PixelSet              missingBase_, addConflicts_, removeConflicts_;
  bool                  paintValue_ = true;
};

// Lays widgets out left to right, wrapping to the available width.
class FlowLayout : public QLayout {
public:
  explicit FlowLayout(QWidget *parent = nullptr) : QLayout(parent) { setSpacing(8); }
  ~FlowLayout() override {
    while (auto *item = takeAt(0)) delete item;
  }
  void addItem(QLayoutItem *item) override { items_.append(item); }
  int  count() const override { return items_.size(); }
  QLayoutItem *itemAt(int index) const override { return items_.value(index); }
  QLayoutItem *takeAt(int index) override {
    return (index >= 0 && index < items_.size()) ? items_.takeAt(index) : nullptr;
  }
  Qt::Orientations expandingDirections() const override { return {}; }
  bool             hasHeightForWidth() const override { return true; }
  int heightForWidth(int width) const override { return doLayout(QRect(0, 0, width, 0), true); }
  void setGeometry(const QRect &rect) override {
    QLayout::setGeometry(rect);
    doLayout(rect, false);
  }
  QSize sizeHint() const override { return minimumSize(); }
  QSize minimumSize() const override {
    QSize size;
    for (auto *item : items_) size = size.expandedTo(item->minimumSize());
    auto m = contentsMargins();
    return size + QSize(m.left() + m.right(), m.top() + m.bottom());
  }

private:
  int doLayout(const QRect &rect, bool testOnly) const {
    auto  m         = contentsMargins();
    QRect area      = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
    int   x         = area.x();
    int   y         = area.y();
    int   rowHeight = 0;
    for (auto *item : items_) {
      if (item->isEmpty()) continue;
      QSize hint = item->sizeHint();
      if ((x + hint.width() > area.right() + 1) && (rowHeight > 0)) {
        x         = area.x();
        y         = y + rowHeight + spacing();
        rowHeight = 0;
      }
      if (!testOnly) item->setGeometry(QRect(QPoint(x, y), hint));
      x         = x + hint.width() + spacing();
      rowHeight = std::max(rowHeight, hint.height());
    }
    return y + rowHeight - rect.y() + m.bottom();
  }

  QList<QLayoutItem *> items_;
};

} // namespace

auto LinkedGlyphProposal::build(BitmapPtr &bitmap, GlyphInfoPtr &info) const -> bool {
  return fromPixels(after, templateInfo, bitmap, info);
}

namespace LinkedGlyphs {

auto propose(IBMFFontModPtr font, int faceIdx, GlyphCode baseGlyphCode, const Bitmap &oldBase,
             const GlyphInfo &oldBaseInfo, const Bitmap &newBase, const GlyphInfo &newBaseInfo,
             IBMFFontModPtr reference, int referenceFaceIdx) -> std::vector<LinkedGlyphProposal> {
  auto links = linkedGlyphCodes(font, faceIdx);
  auto it    = links.find(baseGlyphCode);
  if (it == links.end()) return {};
  return proposeFor(font, faceIdx, baseGlyphCode, it->second, oldBase, oldBaseInfo, newBase,
                    newBaseInfo, reference, referenceFaceIdx);
}

auto proposeFromReference(IBMFFontModPtr font, IBMFFontModPtr reference)
    -> std::vector<LinkedGlyphProposal> {
  std::vector<LinkedGlyphProposal> proposals;
  for (int faceIdx = 0; faceIdx < font->getPreamble().faceCount; faceIdx++) {
    int refFaceIdx = findReferenceFace(font, faceIdx, reference);
    if (refFaceIdx < 0) continue;
    for (auto &[base, linked] : linkedGlyphCodes(font, faceIdx)) {
      GlyphCode refBase = reference->translate(font->getUTF32(base));
      if (!isValidGlyphCode(refBase)) continue;
      GlyphInfoPtr    curInfo, refInfo;
      BitmapPtr       curBitmap, refBitmap;
      GlyphLigKernPtr lk;
      if (!font->getGlyph(faceIdx, base, curInfo, curBitmap, lk) ||
          !reference->getGlyph(refFaceIdx, refBase, refInfo, refBitmap, lk)) {
        continue;
      }
      auto more = proposeFor(font, faceIdx, base, linked, *refBitmap, *refInfo, *curBitmap,
                             *curInfo, reference, refFaceIdx);
      proposals.insert(proposals.end(), more.begin(), more.end());
    }
  }
  return proposals;
}

} // namespace LinkedGlyphs

LinkedGlyphsDialog::LinkedGlyphsDialog(std::vector<LinkedGlyphProposal> proposals,
                                       IBMFFontModPtr font, const QString &title, ApplyFn apply,
                                       QWidget *parent)
    : QDialog(parent), apply_(std::move(apply)) {
  setWindowTitle(title);
  resize(1100, 800);

  std::stable_sort(proposals.begin(), proposals.end(), [](auto &a, auto &b) {
    return std::make_tuple(a.baseCodePoint, a.faceIdx, a.codePoint) <
           std::make_tuple(b.baseCodePoint, b.faceIdx, b.codePoint);
  });

  auto *layout = new QVBoxLayout(this);
  auto *intro  = new QLabel(
      "Each letter gets the same pixel changes as its base letter, leaving accents alone. "
      "<span style='color:#006eff'>■</span> added &nbsp; "
      "<span style='color:#ffafaf'>■</span> removed &nbsp; "
      "<span style='color:#00963c'>■</span> your edit &nbsp; "
      "<span style='color:#ff8c00'>□</span> mismatch (base pixel missing, adding a pixel that is "
      "already on, or removing one that is off). "
      "Click or drag on a right-hand preview to fix pixels before applying.",
      this);
  intro->setWordWrap(true);
  layout->addWidget(intro);

  auto *scroll  = new QScrollArea(this);
  auto *content = new QWidget(scroll);
  auto *groups  = new QVBoxLayout(content);

  QWidget     *group     = nullptr;
  FlowLayout  *grid      = nullptr;
  char32_t     groupBase = 0;

  for (auto &p : proposals) {
    if ((group == nullptr) || (p.baseCodePoint != groupBase)) {
      groupBase           = p.baseCodePoint;
      group               = new QWidget(content);
      auto *groupLayout   = new QVBoxLayout(group);
      auto *header        = new QLabel(
          QString("<span style='font-size:26pt'>%1</span>&nbsp;&nbsp;"
                         "<b>%2</b>&nbsp;&nbsp;<span style='color:#777'>U+%3</span>")
              .arg(codePointString(groupBase).toHtmlEscaped())
              .arg(describe(groupBase).toHtmlEscaped())
              .arg(uint(groupBase), 4, 16, QChar('0')),
          group);
      groupLayout->addWidget(header);
      auto *cells = new QWidget(group);
      grid        = new FlowLayout(cells);
      groupLayout->addWidget(cells);
      groups->addWidget(group);
    }

    auto  entry    = std::make_unique<Entry>();
    entry->proposal = p;
    entry->group    = group;
    Entry *e        = entry.get();

    auto *cell       = new QFrame(grid->parentWidget());
    cell->setFrameShape(QFrame::StyledPanel);
    auto *cellLayout = new QVBoxLayout(cell);

    auto *titleRow = new QHBoxLayout();
    auto *glyph    = new QLabel(codePointString(p.codePoint), cell);
    QFont big      = glyph->font();
    big.setPointSize(30);
    glyph->setFont(big);
    glyph->setMinimumWidth(44);
    glyph->setAlignment(Qt::AlignCenter);
    titleRow->addWidget(glyph);
    auto *name = new QLabel(QString("%1<br><span style='color:#777'>U+%2 · %3 pt</span>")
                                .arg(describe(p.codePoint).toHtmlEscaped())
                                .arg(uint(p.codePoint), 4, 16, QChar('0'))
                                .arg(font->getFaceHeader(p.faceIdx)->pointSize),
                            cell);
    name->setWordWrap(true);
    titleRow->addWidget(name, 1);
    cellLayout->addLayout(titleRow);

    auto *check = new QCheckBox("Include in Apply Selected", cell);
    check->setChecked(p.recommended);
    e->check = check;
    cellLayout->addWidget(new ComparisonWidget(
        &e->proposal, [check] { check->setChecked(true); }, cell));

    if (!p.note.isEmpty()) {
      auto *note = new QLabel(p.note, cell);
      note->setStyleSheet(p.recommended ? "color: #666" : "color: #b00");
      note->setWordWrap(true);
      cellLayout->addWidget(note);
    }

    auto *actions = new QHBoxLayout();
    actions->addWidget(check);
    actions->addStretch(1);
    auto *applyButton = new QPushButton("Apply", cell);
    connect(applyButton, &QPushButton::clicked, this, [this, e] { applyEntry(*e); });
    actions->addWidget(applyButton);
    cellLayout->addLayout(actions);
    cellLayout->addStretch(1);

    e->cell = cell;
    cell->setFixedWidth(std::max(280, cell->sizeHint().width()));
    grid->addWidget(cell);
    entries_.push_back(std::move(entry));
  }
  groups->addStretch(1);

  scroll->setWidget(content);
  scroll->setWidgetResizable(true);
  layout->addWidget(scroll, 1);

  auto *buttons    = new QDialogButtonBox(this);
  auto *selectAll  = buttons->addButton("Select All", QDialogButtonBox::ResetRole);
  auto *selectNone = buttons->addButton("Select None", QDialogButtonBox::ResetRole);
  auto *applyAll   = buttons->addButton("Apply Selected", QDialogButtonBox::AcceptRole);
  buttons->addButton(QDialogButtonBox::Close);
  applyAll->setDefault(true);
  connect(selectAll, &QPushButton::clicked, this, [this] {
    for (auto &e : entries_) e->check->setChecked(true);
  });
  connect(selectNone, &QPushButton::clicked, this, [this] {
    for (auto &e : entries_) e->check->setChecked(false);
  });
  connect(buttons, &QDialogButtonBox::accepted, this, [this] { applySelected(); });
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);
}

auto LinkedGlyphsDialog::applyEntry(Entry &entry) -> void {
  if (entry.done || !apply_ || !apply_(entry.proposal)) return;
  entry.done = true;
  entry.cell->hide();
  bool groupDone = std::all_of(entries_.begin(), entries_.end(), [&](auto &e) {
    return (e->group != entry.group) || e->done;
  });
  if (groupDone) entry.group->hide();
  if (std::all_of(entries_.begin(), entries_.end(), [](auto &e) { return e->done; })) accept();
}

auto LinkedGlyphsDialog::applySelected() -> void {
  for (auto &e : entries_) {
    if (!e->done && e->check->isChecked()) applyEntry(*e);
  }
  accept();
}
