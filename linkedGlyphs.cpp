#include "linkedGlyphs.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <algorithm>
#include <climits>
#include <map>
#include <set>
#include <utility>

namespace {

// Pixel coordinates relative to the glyph origin: x grows right, y grows down, and
// y = -verticalOffset is the bitmap's top row.
using PixelSet = std::set<std::pair<int, int>>;

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

auto proposeFor(IBMFFontModPtr font, int faceIdx, GlyphCode baseGlyphCode,
                const std::vector<GlyphCode> &linked, const Bitmap &oldBase,
                const GlyphInfo &oldBaseInfo, const Bitmap &newBase,
                const GlyphInfo &newBaseInfo, IBMFFontModPtr reference, int referenceFaceIdx)
    -> std::vector<LinkedGlyphProposal> {
  std::vector<LinkedGlyphProposal> proposals;

  PixelSet oldPixels    = toPixels(oldBase, oldBaseInfo);
  PixelSet newPixels    = toPixels(newBase, newBaseInfo);
  FIX16    advanceDelta = newBaseInfo.advance - oldBaseInfo.advance;
  PixelSet added, removed;
  std::set_difference(newPixels.begin(), newPixels.end(), oldPixels.begin(), oldPixels.end(),
                      std::inserter(added, added.end()));
  std::set_difference(oldPixels.begin(), oldPixels.end(), newPixels.begin(), newPixels.end(),
                      std::inserter(removed, removed.end()));
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
    for (int candidate : {0, -1, 1, -2, 2, -3, 3}) {
      if (containsAll(current, shifted(oldPixels, candidate))) {
        dx    = candidate;
        found = true;
        break;
      }
    }

    if (!found) {
      // Skip glyphs that already carry the new base shape (e.g. edited by hand).
      bool alreadyApplied = false;
      for (int candidate : {0, -1, 1, -2, 2, -3, 3}) {
        if (containsAll(current, shifted(newPixels, candidate)) &&
            containsNone(current, shifted(removed, candidate))) {
          alreadyApplied = true;
          break;
        }
      }
      if (alreadyApplied) continue;

      int best = -1;
      for (int candidate : {0, -1, 1, -2, 2, -3, 3}) {
        int score = overlap(current, shifted(oldPixels, candidate));
        if (score > best) {
          best = score;
          dx   = candidate;
        }
      }
    }

    PixelSet result = current;
    for (auto &p : shifted(removed, dx)) result.erase(p);
    for (auto &p : shifted(added, dx)) result.insert(p);
    if ((result == current) && (advanceDelta == 0)) continue;

    LinkedGlyphProposal proposal;
    proposal.faceIdx       = faceIdx;
    proposal.glyphCode     = g;
    proposal.codePoint     = font->getUTF32(g);
    proposal.baseCodePoint = font->getUTF32(baseGlyphCode);
    proposal.oldInfo       = info;
    proposal.oldBitmap     = bitmap;
    proposal.ligKern       = lk;
    proposal.recommended   = found;

    GlyphInfo templateInfo = *info;
    templateInfo.advance   = info->advance + advanceDelta;
    if (!fromPixels(result, templateInfo, proposal.newBitmap, proposal.newInfo)) continue;

    if (!found) {
      proposal.note = "Old base shape not found intact; check carefully";
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

auto renderComparison(const LinkedGlyphProposal &p) -> QPixmap {
  constexpr int SCALE = 6;
  constexpr int GAP   = 3; // in glyph pixels, between the before and after panels

  PixelSet before = toPixels(*p.oldBitmap, *p.oldInfo);
  PixelSet after  = toPixels(*p.newBitmap, *p.newInfo);
  PixelSet all    = before;
  all.insert(after.begin(), after.end());

  int minX = 0, maxX = 0, minY = 0, maxY = 0;
  if (!all.empty()) {
    minX = minY = INT_MAX;
    maxX = maxY = INT_MIN;
    for (auto &q : all) {
      minX = std::min(minX, q.first);
      maxX = std::max(maxX, q.first);
      minY = std::min(minY, q.second);
      maxY = std::max(maxY, q.second);
    }
  }
  minX -= 1;
  maxX += 1;
  minY -= 1;
  maxY += 1;
  int panelW = maxX - minX + 1, panelH = maxY - minY + 1;

  QImage image((panelW * 2 + GAP) * SCALE, panelH * SCALE, QImage::Format_RGB32);
  image.fill(Qt::white);
  QPainter painter(&image);

  auto drawPanel = [&](int panel, auto colorFor) {
    int originX = panel * (panelW + GAP);
    for (int y = minY; y <= maxY; y++) {
      for (int x = minX; x <= maxX; x++) {
        QColor color = colorFor(std::make_pair(x, y));
        QRect  cell((originX + x - minX) * SCALE, (y - minY) * SCALE, SCALE, SCALE);
        painter.fillRect(cell, color);
        painter.setPen(QColor(235, 235, 235));
        painter.drawRect(cell.adjusted(0, 0, -1, -1));
      }
    }
  };

  drawPanel(0, [&](auto q) { return before.count(q) ? QColor(Qt::black) : QColor(Qt::white); });
  drawPanel(1, [&](auto q) {
    bool wasOn = before.count(q) != 0, isOn = after.count(q) != 0;
    if (isOn && !wasOn) return QColor(0, 110, 255);
    if (wasOn && !isOn) return QColor(255, 175, 175);
    return isOn ? QColor(Qt::black) : QColor(Qt::white);
  });

  painter.setPen(QPen(QColor(120, 120, 120), 2));
  int midY   = image.height() / 2;
  int arrowL = (panelW * SCALE) + SCALE / 2;
  int arrowR = (panelW + GAP) * SCALE - SCALE / 2;
  painter.drawLine(arrowL, midY, arrowR, midY);
  painter.drawLine(arrowR, midY, arrowR - SCALE, midY - SCALE);
  painter.drawLine(arrowR, midY, arrowR - SCALE, midY + SCALE);
  return QPixmap::fromImage(image);
}

} // namespace

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
                                       IBMFFontModPtr font, const QString &title, QWidget *parent)
    : QDialog(parent), proposals_(std::move(proposals)) {
  setWindowTitle(title);
  resize(900, 640);

  auto *layout = new QVBoxLayout(this);
  auto *intro  = new QLabel(
      "Replay the base letter's pixel changes onto its linked letters. "
      "<span style='color:#006eff'>Blue</span> pixels are added, "
      "<span style='color:#d77'>pink</span> pixels are removed. "
      "Unchecked items need a closer look.",
      this);
  intro->setWordWrap(true);
  layout->addWidget(intro);

  auto *scroll  = new QScrollArea(this);
  auto *content = new QWidget(scroll);
  auto *grid    = new QGridLayout(content);
  constexpr int COLUMNS = 4;

  for (size_t i = 0; i < proposals_.size(); i++) {
    auto &p     = proposals_[i];
    auto *cell  = new QFrame(content);
    cell->setFrameShape(QFrame::StyledPanel);
    auto *cellLayout = new QVBoxLayout(cell);

    auto *check = new QCheckBox(QString("%1  U+%2  ← %3  ·  %4 pt")
                                    .arg(codePointString(p.codePoint))
                                    .arg(uint(p.codePoint), 4, 16, QChar('0'))
                                    .arg(codePointString(p.baseCodePoint))
                                    .arg(font->getFaceHeader(p.faceIdx)->pointSize),
                                cell);
    check->setChecked(p.recommended);
    checks_.push_back(check);
    cellLayout->addWidget(check);

    auto *image = new QLabel(cell);
    image->setPixmap(renderComparison(p));
    cellLayout->addWidget(image);

    if (!p.note.isEmpty()) {
      auto *note = new QLabel(p.note, cell);
      note->setStyleSheet(p.recommended ? "color: #666" : "color: #b00");
      note->setWordWrap(true);
      cellLayout->addWidget(note);
    }
    grid->addWidget(cell, static_cast<int>(i) / COLUMNS, static_cast<int>(i) % COLUMNS);
  }

  scroll->setWidget(content);
  scroll->setWidgetResizable(true);
  layout->addWidget(scroll, 1);

  auto *buttons     = new QDialogButtonBox(this);
  auto *selectAll   = buttons->addButton("Select All", QDialogButtonBox::ResetRole);
  auto *selectNone  = buttons->addButton("Select None", QDialogButtonBox::ResetRole);
  auto *apply       = buttons->addButton("Apply Selected", QDialogButtonBox::AcceptRole);
  buttons->addButton(QDialogButtonBox::Cancel);
  apply->setDefault(true);
  connect(selectAll, &QPushButton::clicked, this, [this] {
    for (auto *c : checks_) c->setChecked(true);
  });
  connect(selectNone, &QPushButton::clicked, this, [this] {
    for (auto *c : checks_) c->setChecked(false);
  });
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);
}

auto LinkedGlyphsDialog::selectedProposals() const -> std::vector<LinkedGlyphProposal> {
  std::vector<LinkedGlyphProposal> selected;
  for (size_t i = 0; i < proposals_.size(); i++) {
    if (checks_[i]->isChecked()) selected.push_back(proposals_[i]);
  }
  return selected;
}
