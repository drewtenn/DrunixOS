-- strip-latex.lua
-- Pandoc Lua filter for epub output.
--   1. Removes LaTeX-specific constructs (\newpage, raw TeX blocks, etc.)
--   2. Rewrites .svg image paths to .png so Apple Books can tap-to-zoom.
--      (Apple Books only supports zoom on raster images, not SVG.)

function RawBlock(el)
  if el.format == "latex" or el.format == "tex" then
    return {}
  end
end

function RawInline(el)
  if el.format == "latex" or el.format == "tex" then
    return pandoc.Str("")
  end
end

function Image(el)
  el.src = el.src:gsub("%.svg$", ".png")
  return el
end
