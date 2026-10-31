#pragma once

#include <QDialog>
#include <QString>
#include <vector>

#include "IBMFDriver/IBMFFontMod.hpp"

class QCheckBox;

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
  IBMFDefs::GlyphInfoPtr    newInfo;
  IBMFDefs::BitmapPtr       newBitmap;
  IBMFDefs::GlyphLigKernPtr ligKern;
  QString                   note;
  bool                      recommended;
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

class LinkedGlyphsDialog : public QDialog {
public:
  LinkedGlyphsDialog(std::vector<LinkedGlyphProposal> proposals, IBMFFontModPtr font,
                     const QString &title, QWidget *parent = nullptr);

  auto selectedProposals() const -> std::vector<LinkedGlyphProposal>;

private:
  std::vector<LinkedGlyphProposal> proposals_;
  std::vector<QCheckBox *>         checks_;
};
