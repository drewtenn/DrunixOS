include docs/sources.mk

EPUB_FRONTMATTER := docs/epub-copyright.md
PDF_FRONTMATTER := docs/epub-copyright.md
PDF_METADATA := docs/pdf-metadata.yaml
PDF_FILTER := docs/pdf.lua
PDF_TEMPLATE := docs/pdf-template.typ

PDF  := docs/Drunix OS.pdf
EPUB := docs/Drunix OS.epub

# docs/generate_diagrams.py is the source of truth for generated diagrams.
# It writes SVG files into docs/diagrams/. docs/extract-diagrams.py remains
# as the one-time ASCII-art migration helper.
#
# epub build: SVGs are converted to PNG (2x scale) so Apple Books can
#             tap-to-zoom. The Lua filter rewrites .svg paths to .png.
# PDF build:  Pandoc renders the markdown sources through Typst for stable
#             book typography while keeping SVG diagrams vector-sharp.

DIAGRAMS_SVG := $(wildcard docs/diagrams/*.svg)
DIAGRAMS_PNG := $(DIAGRAMS_SVG:.svg=.png)

docs/diagrams/%.png: docs/diagrams/%.svg
	@if command -v rsvg-convert >/dev/null 2>&1; then \
		rsvg-convert -f png -z 2 -o "$@" "$<"; \
	elif $(PYTHON) -c "import cairosvg" >/dev/null 2>&1; then \
		$(PYTHON) -c "import cairosvg; cairosvg.svg2png(url='$<', write_to='$@', scale=2)"; \
	else \
		echo "error: need the 'rsvg-convert' binary from librsvg or Python package 'cairosvg' to build EPUB diagrams" >&2; \
		exit 1; \
	fi

docs/Drunix\ OS.pdf: $(PDF_FRONTMATTER) $(DOCS_SRC) $(DIAGRAMS_SVG) docs/cover-art.png docs/epub-metadata.yaml $(PDF_METADATA) $(PDF_FILTER) $(PDF_TEMPLATE)
	pandoc $(PDF_FRONTMATTER) $(DOCS_SRC) \
	    --to typst \
	    --standalone \
	    --template "$(PDF_TEMPLATE)" \
	    --metadata-file docs/epub-metadata.yaml \
	    --metadata-file "$(PDF_METADATA)" \
	    --lua-filter "$(PDF_FILTER)" \
	    --syntax-highlighting=monochrome \
	    --resource-path=docs \
	    --pdf-engine=typst \
	    -o "$(PDF)"

docs/Drunix\ OS.epub: $(EPUB_FRONTMATTER) $(DOCS_SRC) $(DIAGRAMS_PNG) docs/cover-art.png docs/epub.css docs/epub-metadata.yaml docs/strip-latex.lua
	tmpdir="$$(mktemp -d /tmp/drunix-epub.XXXXXX)"; \
	pandoc $(EPUB_FRONTMATTER) $(DOCS_SRC) \
	    --to epub3 \
	    --toc \
	    --toc-depth=2 \
	    --split-level=2 \
	    --epub-title-page=false \
	    --epub-cover-image=docs/cover-art.png \
	    --css docs/epub.css \
	    --metadata-file docs/epub-metadata.yaml \
	    --lua-filter docs/strip-latex.lua \
	    --syntax-highlighting=monochrome \
	    --resource-path=docs \
	    -o "$$tmpdir/book.epub"; \
	cd "$$tmpdir" && unzip -q book.epub -d unpacked; \
	perl -0pi -e 's|<itemref idref="cover_xhtml" />\s*<itemref idref="nav" />\s*<itemref idref="ch001_xhtml" />|<itemref idref="cover_xhtml" />\n    <itemref idref="ch001_xhtml" />\n    <itemref idref="nav" />|s' "$$tmpdir/unpacked/EPUB/content.opf"; \
	rm -f "$(EPUB)"; \
	cd "$$tmpdir/unpacked" && zip -X0 "../book-fixed.epub" mimetype >/dev/null && zip -Xr9D "../book-fixed.epub" META-INF EPUB >/dev/null; \
	mv "$$tmpdir/book-fixed.epub" "$(CURDIR)/$(EPUB)"; \
	rm -rf "$$tmpdir"

pdf: docs/Drunix\ OS.pdf

epub: docs/Drunix\ OS.epub

docs: pdf epub
