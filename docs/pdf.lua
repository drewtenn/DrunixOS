-- pdf.lua
-- Pandoc Lua filter for Typst PDF output.
--   1. Converts LaTeX page-break markers used by chapters to Typst.
--   2. Drops any other raw TeX so the Typst renderer sees clean input.

local function is_tex(format)
  return format == "latex" or format == "tex"
end

local function is_pagebreak(text)
  return text:match("^%s*\\newpage%s*$") or text:match("^%s*\\clearpage%s*$")
end

function RawBlock(el)
  if is_tex(el.format) then
    if is_pagebreak(el.text) then
      return pandoc.RawBlock("typst", "#pagebreak()")
    end

    return {}
  end
end

function RawInline(el)
  if is_tex(el.format) then
    return pandoc.Str("")
  end
end
