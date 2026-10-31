#pragma once

#include <QDialog>
#include <QString>
#include <functional>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "IBMFDriver/IBMFFontMod.hpp"

class QCheckBox;

namespace LinkedGlyphs {
// Pixel coordinates relative to the glyph origin: x grows right, y grows down, and
// y = -verticalOffset is the bitmap's top row.
using PixelSet = std::set<std::pair<int, int>>;
} // namespace LinkedGlyphs

// A proposed update to a glyph linked (via GlyphInfo::mainCode) to an edited base glyph,
// e.g. "é" linked to "e". The base glyph's added/removed pixels are replayed onto the
// linked glyph, so its accent is left untouched.
struct LinkedGlyphProposal {
  int                       faceIdx;
  IBMFDefs::GlyphCode       glyphCode;
  char32_t                  codePoint;
  char32_t                  baseCodePoint;
  IBMFDefs::GlyphInfoPtr    oldInfo;
  IBMFDefs::BitmapPtr       oldBitmap;
  IBMFDefs::GlyphLigKernPtr ligKern;
  IBMFDefs::GlyphInfo       templateInfo; // Linked glyph info carrying the new advance

  LinkedGlyphs::PixelSet before;       // Linked glyph as it is now
  LinkedGlyphs::PixelSet computed;     // Result of replaying the base change
  LinkedGlyphs::PixelSet after;        // Result, including edits made in the review dialog
  LinkedGlyphs::PixelSet expectedBase; // Old base pixels at the matched position
  LinkedGlyphs::PixelSet added;        // Base pixels added, at the matched position
  LinkedGlyphs::PixelSet removed;      // Base pixels removed, at the matched position

  QString note;
  bool    recommended;

  // Bitmap and metrics for `after`.
  auto build(IBMFDefs::BitmapPtr &bitmap, IBMFDefs::GlyphInfoPtr &info) const -> bool;
};

namespace LinkedGlyphs {

// Proposals for every glyph linked to baseGlyphCode in faceIdx, given the base glyph's
// before/after versions. When a reference font is supplied, linked glyphs that differ from
// their reference version are flagged as already edited and not recommended.
auto propose(IBMFFontModPtr font, int faceIdx, IBMFDefs::GlyphCode baseGlyphCode,
             const IBMFDefs::Bitmap &oldBase, const IBMFDefs::GlyphInfo &oldBaseInfo,
             const IBMFDefs::Bitmap &newBase, const IBMFDefs::GlyphInfo &newBaseInfo,
             IBMFFontModPtr reference = nullptr, int referenceFaceIdx = -1)
    -> std::vector<LinkedGlyphProposal>;

// Catch-up: compares every linked base glyph against a reference font (typically the font
// before any hand edits) and proposes replaying each base glyph's change onto its linked glyphs.
auto proposeFromReference(IBMFFontModPtr font, IBMFFontModPtr reference)
    -> std::vector<LinkedGlyphProposal>;

} // namespace LinkedGlyphs

// Review dialog: proposals grouped by base letter, each with an editable after-preview and its
// own Apply button. `apply` commits one proposal to the font and returns false on failure.
class LinkedGlyphsDialog : public QDialog {
public:
  using ApplyFn = std::function<bool(const LinkedGlyphProposal &)>;

  LinkedGlyphsDialog(std::vector<LinkedGlyphProposal> proposals, IBMFFontModPtr font,
                     const QString &title, ApplyFn apply, QWidget *parent = nullptr);

private:
  struct Entry {
    LinkedGlyphProposal proposal;
    QCheckBox          *check = nullptr;
    QWidget            *cell  = nullptr;
    QWidget            *group = nullptr;
    bool                done  = false;
  };

  auto applyEntry(Entry &entry) -> void;
  auto applySelected() -> void;

  ApplyFn                             apply_;
  std::vector<std::unique_ptr<Entry>> entries_;
};
