// Custom Typst template for the Drunix book PDF.
// Replaces pandoc's default conf wrapper with a layout that:
//   1. Places cover-art.png as a chromeless edge-to-edge page 1.
//   2. Places the table of contents alone on page 2, no header/footer/numbering.
//   3. Resets page numbering so chapter content starts at page 1 with normal margins.
//
// This file is referenced from the Makefile's PDF rule via --template.

#set document(
  title: "Drunix OS",
  author: "Drew Tennenbaum",
)

#set text(size: $if(fontsize)$$fontsize$$else$10.5pt$endif$, lang: "en", region: "US")
#set par(justify: true, leading: $if(linestretch)$$linestretch$$else$1.08$endif$ * 0.65em)

#let horizontalrule = line(start: (25%,0%), end: (75%,0%))

#show terms.item: it => block(breakable: false)[
  #text(weight: "bold")[#it.term]
  #block(inset: (left: 1.5em, top: -0.4em))[#it.description]
]

#set table(inset: 6pt, stroke: none)

#show figure.where(kind: table): set figure.caption(position: top)
#show figure.where(kind: image): set figure.caption(position: bottom)

$if(highlighting-definitions)$
$highlighting-definitions$
$endif$

// ── Cover ─────────────────────────────────────────────────────────────────────
#set page(
  paper: "us-letter",
  margin: 0pt,
  header: none,
  footer: none,
  numbering: none,
)
#align(center + horizon)[
  #image("docs/cover-art.png", width: 100%, height: 100%, fit: "contain")
]

// ── Table of contents (own page, no chrome) ───────────────────────────────────
#set page(
  paper: "us-letter",
  margin: (top: 0.75in, bottom: 0.85in, left: 0.9in, right: 0.9in),
  header: none,
  footer: none,
  numbering: none,
)
#pagebreak(weak: true)
#outline(title: [Contents], depth: 2)

// ── Book body ─────────────────────────────────────────────────────────────────
#set page(
  paper: "us-letter",
  margin: (top: 0.75in, bottom: 0.85in, left: 0.9in, right: 0.9in),
  numbering: "1",
)
#counter(page).update(1)
#pagebreak(weak: true)

// Section heading style — keep pandoc defaults light.
#set heading(numbering: none)

$body$
