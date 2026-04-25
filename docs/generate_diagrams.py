#!/usr/bin/env python3

"""Generate the clean SVG diagrams used by the book."""

from dataclasses import dataclass
from pathlib import Path
import html, re, textwrap

DOCS = Path(__file__).resolve().parent
OUT = DOCS / 'diagrams'
FONT = '-apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif'
MONO = 'ui-monospace, "SFMono-Regular", Menlo, Consolas, monospace'
PANEL_X = 44
PANEL_Y = 98
PANEL_W = 672
PANEL_PAD_Y = 12
SVG_OUTER_PAD_X = 34
TITLE_INSET_X = 10
TITLE_BASELINE_Y = 42
SUBTITLE_BASELINE_Y = 68
SUBTITLE_LINE_GAP = 16
HEADER_TO_PANEL_GAP = 26
PANEL_BOTTOM_MARGIN = 48
CONTENT_PAD_X_RATIO = 0.05
CONTENT_PAD_Y_RATIO = 0.11
GENERATED = []
BOX_TEXT_PAD_X = 24
BOX_TEXT_PAD_Y = 18
BOX_LINE_GAP = 16
STACK_BOX_WIDTH_RATIO = 0.72
STACK_EDGE_PAD_X = 34
STACK_EDGE_PAD_Y = 26
STACK_ROW_GAP = 18
STACK_NOTE_H = 22
SEQUENCE_PANEL_PAD_X = 34
SEQUENCE_PANEL_PAD_Y = 26
TREE_PANEL_PAD_X = 34
TREE_PANEL_PAD_Y = 26
STATE_PANEL_PAD_X = 34
STATE_PANEL_PAD_Y = 26
STATE_EDGE_STUB = 14
STATE_LABEL_MARGIN = 12
TABLE_PANEL_PAD_X = 28
TABLE_PANEL_PAD_Y = 26
SNAPSHOT_PANEL_PAD_X = 32
SNAPSHOT_PANEL_PAD_Y = 24
COMPARISON_PANEL_PAD_X = 34
COMPARISON_PANEL_PAD_Y = 26
CYCLIC_PANEL_PAD_X = 34
CYCLIC_PANEL_PAD_Y = 26
SEGMENTED_PANEL_PAD_X = 34
SEGMENTED_PANEL_PAD_Y = 26
TRANSFER_PANEL_PAD_X = 34
TRANSFER_PANEL_PAD_Y = 24
LANE_PANEL_PAD_X = 34
LANE_PANEL_PAD_Y = 24
SPLIT_PANEL_PAD_X = 34
SPLIT_PANEL_PAD_Y = 24
CYCLIC_QUEUE_LABEL_H = 16
CYCLIC_QUEUE_TO_CELLS = 16
CYCLIC_CELLS_TO_CURSOR = 18
CYCLIC_CURSOR_LABEL_H = 16
CYCLIC_CURSOR_TO_WRAP = 18
CYCLIC_WRAP_BAND_H = 36
POLY_ARROW_MIN_END_TAIL = 18
SEGMENTED_LABEL_H = 16
SEGMENTED_LABEL_GAP = 14
TRANSCRIPT_EDGE_PAD_X = 34
TRANSCRIPT_EDGE_PAD_Y = 24
TRANSCRIPT_COL_GAP = 24
TRANSCRIPT_MIN_PANEL_W = 420

# Branching-diagram connector rules. These encode the style-guide requirement
# that decision-tree arrows need visibly long shafts rather than cramped turns.
TREE_LABEL_CLEARANCE = 14
TREE_EDGE_CLEARANCE = 12
TREE_SIDE_LABEL_DX = 16
TREE_MIN_VERTICAL_SHAFT = 18
TREE_MIN_VERTICAL_TAIL = 8
TREE_MIN_HORIZONTAL_SHAFT = 24
TREE_MIN_HORIZONTAL_TAIL = 10

# Sequential-diagram connector rules. Straight-line handoff diagrams should
# also keep a visible arrow shaft rather than letting boxes nearly touch.
SEQUENCE_ARROW_START_PAD = 8
SEQUENCE_ARROW_END_PAD = 12
SEQUENCE_MIN_HORIZONTAL_SHAFT = 24
SEQUENCE_MIN_VERTICAL_SHAFT = 18

VALID_BUCKETS = {
    'annotated_example',
    'bitfield',
    'buffer_transfer',
    'grid_snapshot',
    'cyclic_buffer',
    'comparison',
    'decision_tree',
    'handoff_path',
    'layer_stack',
    'layout',
    'lane_walk',
    'segmented_strip',
    'split_overview',
    'snapshot',
    'state_machine',
    'table',
    'tagged_transcript',
    'walk',
}


@dataclass(frozen=True)
class Rect:
    x: float
    y: float
    w: float
    h: float

    @property
    def right(self):
        return self.x + self.w

    @property
    def bottom(self):
        return self.y + self.h

    @property
    def cx(self):
        return self.x + self.w / 2

    @property
    def cy(self):
        return self.y + self.h / 2

    def inset(self, dx, dy=None):
        if dy is None:
            dy = dx
        return Rect(self.x + dx, self.y + dy, self.w - dx * 2, self.h - dy * 2)

    def child(self, rel_x, rel_y, rel_w, rel_h):
        return Rect(
            self.x + self.w * rel_x,
            self.y + self.h * rel_y,
            self.w * rel_w,
            self.h * rel_h,
        )

    def point(self, rel_x, rel_y):
        return (self.x + self.w * rel_x, self.y + self.h * rel_y)

DIAGRAM_BUCKETS = {
    # Chapter 1
    'ch01-diag01.svg': 'snapshot',
    'ch01-diag02.svg': 'layout',
    'ch01-diag03.svg': 'layout',
    'ch01-diag04.svg': 'snapshot',
    # Chapter 2
    # Chapter 3
    'ch03-diag01.svg': 'grid_snapshot',
    # Chapter 4
    'ch04-diag01.svg': 'layout',
    'ch04-diag02.svg': 'table',
    # Chapter 5
    'ch05-diag01.svg': 'handoff_path',
    'ch05-diag02.svg': 'comparison',
    # Chapter 7
    # Chapter 8
    'ch08-diag01.svg': 'layout',
    'ch08-diag02.svg': 'walk',
    'ch08-diag03.svg': 'segmented_strip',
    'ch08-diag04.svg': 'comparison',
    'ch08-diag05.svg': 'comparison',
    'ch08-diag06.svg': 'layout',
    'ch08-diag07.svg': 'walk',
    # Chapter 9
    'ch09-diag01.svg': 'tagged_transcript',
    'ch09-diag02.svg': 'tagged_transcript',
    # Chapter 10
    'ch10-diag01.svg': 'cyclic_buffer',
    # Chapter 11
    # Chapter 12
    # Chapter 13
    'ch13-diag01.svg': 'layout',
    'ch13-diag02.svg': 'walk',
    'ch13-diag03.svg': 'buffer_transfer',
    'ch13-diag04.svg': 'walk',
    'ch13-diag05.svg': 'lane_walk',
    'ch13-diag06.svg': 'segmented_strip',
    'ch13-diag07.svg': 'split_overview',
    'ch13-diag08.svg': 'handoff_path',
    # Chapter 14
    'ch14-diag01.svg': 'layer_stack',
    # Chapter 15
    'ch15-diag01.svg': 'layout',
    'ch15-diag02.svg': 'state_machine',
    'ch15-diag03.svg': 'layout',
    'ch15-diag04.svg': 'comparison',
    # Chapter 16
    'ch16-diag01.svg': 'handoff_path',
    # Chapter 17
    # Chapter 18
    'ch18-diag01.svg': 'handoff_path',
    'ch18-diag02.svg': 'decision_tree',
    # Chapter 19
    'ch19-diag01.svg': 'handoff_path',
    'ch19-diag02.svg': 'layout',
    # Chapter 20
    'ch20-diag01.svg': 'handoff_path',
    # Chapter 21
    'ch21-diag01.svg': 'layer_stack',
    # Chapter 22
    # Chapter 23
    'ch23-diag01.svg': 'handoff_path',
    # Chapter 24
    'ch24-diag01.svg': 'layout',
    # Chapter 25
    'ch25-diag01.svg': 'decision_tree',
    'ch25-diag02.svg': 'handoff_path',
    # Chapter 26
    'ch26-diag01.svg': 'handoff_path',
    'ch26-diag02.svg': 'state_machine',
    # Chapter 28
    'ch28-diag01.svg': 'comparison',
    'ch28-diag02.svg': 'handoff_path',
    'ch28-diag03.svg': 'layer_stack',
    # Chapter 29
    'ch29-diag01.svg': 'handoff_path',
    'ch29-diag02.svg': 'decision_tree',
    # Chapter 30
    'ch30-diag01.svg': 'handoff_path',
    'ch30-diag02.svg': 'layer_stack',
}

def wrap_chars_for_width(px, avg_char_px=6.2):
    return max(24, int(px / avg_char_px))

def esc(s): return html.escape(str(s).replace('`', ''), quote=False)

def figure_label(path):
    match = re.fullmatch(r'ch(\d+)-diag(\d+)\.svg', Path(path).name)
    if not match:
        return ''
    chapter = int(match.group(1))
    figure = int(match.group(2))
    return f'Chapter {chapter}, Figure {figure}'

def add_figure_footer(path, svg_text):
    label = figure_label(path)
    if not label:
        return svg_text
    viewbox_match = re.search(r'viewBox="0 0 ([0-9.]+) ([0-9.]+)"', svg_text)
    if not viewbox_match:
        return svg_text
    w = float(viewbox_match.group(1))
    h = float(viewbox_match.group(2))
    footer = (
        f'  <text class="muted" x="{w - 34:.1f}" y="{h - 18:.1f}" '
        f'text-anchor="end">{esc(label)}</text>\n'
    )
    return svg_text.replace('</svg>\n', footer + '</svg>\n')

def write_svg(path, svg_text):
    out_path = OUT / path
    out_path.write_text(add_figure_footer(path, svg_text))
    GENERATED.append(Path(path).name)

def referenced_diagrams():
    refs = set()
    for md in DOCS.glob('ch*.md'):
        refs.update(re.findall(r'!\[\]\(diagrams/(ch\d+-diag\d+\.svg)\)', md.read_text()))
    return refs

def validate_generated_diagrams():
    refs = referenced_diagrams()
    generated = set(GENERATED)
    on_disk = {p.name for p in OUT.glob('ch*-diag*.svg')}
    invalid_buckets = sorted(set(DIAGRAM_BUCKETS.values()) - VALID_BUCKETS)
    uncategorized = sorted(refs - set(DIAGRAM_BUCKETS))
    stale_bucket_entries = sorted(set(DIAGRAM_BUCKETS) - refs)
    missing = sorted(refs - generated)
    orphaned = sorted(on_disk - refs)
    problems = []
    if invalid_buckets:
        problems.append('Unknown diagram buckets: ' + ', '.join(invalid_buckets))
    if uncategorized:
        problems.append('Referenced but uncategorized: ' + ', '.join(uncategorized))
    if stale_bucket_entries:
        problems.append('Bucket entries without chapter references: ' + ', '.join(stale_bucket_entries))
    if missing:
        problems.append('Referenced but not generated: ' + ', '.join(missing))
    if orphaned:
        problems.append('Generated but unreferenced: ' + ', '.join(orphaned))
    if problems:
        raise SystemExit('\n'.join(problems))

def lines(s, width=24):
    out=[]
    for part in str(s).split('\n'):
        if len(part) <= width:
            out.append(part)
        else:
            out += textwrap.wrap(part, width=width, break_long_words=True) or ['']
    return out

def label_lines(s, width=24):
    return str(s).split('\n')

def estimate_text_width(s, px=7.0, pad=0):
    lines_ = str(s).split('\n')
    longest = max((len(line) for line in lines_), default=0)
    return longest * px + pad

def estimate_box_width(label, sub='', min_w=92, mono=False):
    label_px = 7.2 if not mono else 7.4
    sub_px = 6.5 if not mono else 6.8
    label_w = max((estimate_text_width(line, label_px) for line in label_lines(label)), default=0)
    sub_w = max((estimate_text_width(line, sub_px) for line in label_lines(sub)), default=0)
    return max(min_w, label_w, sub_w) + 28

def subtitle_lines_for_width(subtitle, subtitle_width):
    return lines(subtitle, wrap_chars_for_width(subtitle_width)) if subtitle else []

def subtitle_block_bottom(subtitle, subtitle_width):
    subtitle_lines = subtitle_lines_for_width(subtitle, subtitle_width)
    if not subtitle_lines:
        return SUBTITLE_BASELINE_Y + 4
    return SUBTITLE_BASELINE_Y + (len(subtitle_lines) - 1) * SUBTITLE_LINE_GAP + 4

def panel_top_y(subtitle, subtitle_width):
    return subtitle_block_bottom(subtitle, subtitle_width) + HEADER_TO_PANEL_GAP

def start(title, subtitle='', w=760, h=360, title_x=44, subtitle_x=None, subtitle_width=None):
    if subtitle_x is None:
        subtitle_x = title_x
    if subtitle_width is None:
        subtitle_width = w - subtitle_x - 44
    subtitle_lines = subtitle_lines_for_width(subtitle, subtitle_width)
    subtitle_svg = []
    for i, line in enumerate(subtitle_lines):
        subtitle_svg.append(f'  <text class="muted" x="{subtitle_x}" y="{SUBTITLE_BASELINE_Y + i * SUBTITLE_LINE_GAP}">{esc(line)}</text>')
    return [f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}" role="img" aria-labelledby="title desc">',
            f'  <title id="title">{esc(title)}</title>',
            f'  <desc id="desc">{esc(subtitle or title)}</desc>',
            '  <defs>',
            '    <marker id="arrow" viewBox="0 0 12 12" refX="10" refY="6" markerWidth="6" markerHeight="6" orient="auto">',
            '      <path d="M 2 2 L 10 6 L 2 10" fill="none" stroke="#0f172a" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"/>',
            '    </marker>',
            '    <style>',
            '      .bg { fill: #ffffff; }',
            '      .panel { fill: #ffffff; stroke: #cbd5e1; stroke-width: 1.5; }',
            '      .blue { fill: #e0f2fe; stroke: #0284c7; stroke-width: 1.5; }',
            '      .green { fill: #ecfdf5; stroke: #059669; stroke-width: 1.5; }',
            '      .amber { fill: #fef3c7; stroke: #d97706; stroke-width: 1.5; }',
            '      .gray { fill: #f1f5f9; stroke: #64748b; stroke-width: 1.3; }',
            '      .chip { fill: #e0f2fe; stroke: #0284c7; stroke-width: 1.5; }',
            '      .vector { fill: #ecfdf5; stroke: #059669; stroke-width: 1.5; }',
            '      .tag { fill: #dbeafe; stroke: #2563eb; stroke-width: 1.25; }',
            '      .cell { fill: #e0f2fe; stroke: #0284c7; stroke-width: 1.4; }',
            '      .attr { fill: #e0f2fe; stroke: #0284c7; stroke-width: 1.4; }',
            '      .row { fill: #ffffff; stroke: #cbd5e1; stroke-width: 1.2; }',
            '      .gap { fill: #f1f5f9; stroke: #cbd5e1; stroke-width: 1.2; }',
            '      .table-shell { fill: #ffffff; stroke: #cbd5e1; stroke-width: 1.2; }',
            '      .table-headband { fill: #f8fafc; }',
            '      .table-rule { stroke: #cbd5e1; stroke-width: 1; }',
            '      .edge-label-bg { fill: #ffffff; fill-opacity: 0.94; }',
            f'      .title {{ fill: #0f172a; font: 700 18px {FONT}; }}',
            f'      .label {{ fill: #0f172a; font: 600 14px {FONT}; }}',
            f'      .label-tight {{ fill: #0f172a; font: 600 12px {FONT}; }}',
            f'      .label-mini {{ fill: #0f172a; font: 600 11px {FONT}; }}',
            f'      .label-micro {{ fill: #0f172a; font: 600 10px {FONT}; }}',
            f'      .colhead {{ fill: #334155; font: 700 13px {FONT}; }}',
            f'      .small {{ fill: #334155; font: 13px {FONT}; }}',
            f'      .muted {{ fill: #64748b; font: 12px {FONT}; }}',
            f'      .mono {{ fill: #0f172a; font: 12px {MONO}; }}',
            f'      .byte {{ fill: #0f172a; font: 600 12px {MONO}; text-anchor: middle; dominant-baseline: middle; }}',
            '      .arrow { stroke: #0f172a; stroke-width: 2.2; stroke-linecap: round; marker-end: url(#arrow); }',
            '      .line { stroke: #334155; stroke-width: 1.6; stroke-linecap: round; }',
            '    </style>', '  </defs>',
            f'  <rect class="bg" x="0" y="0" width="{w}" height="{h}" rx="12"/>',
            f'  <text class="title" x="{title_x}" y="{TITLE_BASELINE_Y}">{esc(title)}</text>',
            *subtitle_svg]

def finish(a):
    a.append('</svg>')
    return '\n'.join(x for x in a if x!='') + '\n'

def text(a, x,y,s, cls='small', anchor='start'):
    a.append(f'  <text class="{cls}" x="{x}" y="{y}" text-anchor="{anchor}">{esc(s)}</text>')

def box(a,x,y,w,h,label,cls='blue',sub='',mono=False):
    a.append(f'  <rect class="{cls}" x="{x}" y="{y}" width="{w}" height="{h}" rx="7"/>')
    label_width=max(8, int(w/9))
    sub_width=max(10, int(w/8))
    lns=label_lines(label, label_width)
    sublns=lines(sub, sub_width) if sub else []
    if mono:
        label_class = "mono"
    else:
        longest = max(len(ln) for ln in lns) if lns else 0
        est = longest * 7.2
        if est <= w - BOX_TEXT_PAD_X:
            label_class = "label"
        elif longest * 6.2 <= w - (BOX_TEXT_PAD_X - 2):
            label_class = "label-tight"
        elif longest * 5.6 <= w - (BOX_TEXT_PAD_X - 4):
            label_class = "label-mini"
        else:
            label_class = "label-micro"
    text_rows = [(ln, label_class) for ln in lns]
    text_rows += [(ln, "muted") for ln in sublns]
    cy = y + h / 2 - (len(text_rows) - 1) * BOX_LINE_GAP / 2
    for ln, klass in text_rows:
        a.append(f'  <text class="{klass}" x="{x+w/2:.1f}" y="{cy:.1f}" text-anchor="middle" dominant-baseline="middle">{esc(ln)}</text>')
        cy += BOX_LINE_GAP

def box_height(label, sub='', width=210, mono=False):
    label_width=max(8, int(width/9))
    sub_width=max(10, int(width/8))
    lns=label_lines(label, label_width)
    sublns=lines(sub, sub_width) if sub else []
    rows = len(lns) + len(sublns)
    return max(56, rows * BOX_LINE_GAP + BOX_TEXT_PAD_Y * 2)

def landscape_box_dims(label, sub='', min_w=92, preferred_w=None, mono=False):
    width = max(min_w, preferred_w or min_w, estimate_box_width(label, sub, min_w=min_w, mono=mono))
    for _ in range(12):
        height = box_height(label, sub, width, mono=mono)
        if width >= height:
            return width, height
        width = max(width + 8, height)
    return width, box_height(label, sub, width, mono=mono)

def arrow(a,x1,y1,x2,y2):
    a.append(f'  <line class="arrow" x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}"/>')

def panel(a,x=44,y=98,w=672,h=230):
    a.append(f'  <rect class="panel" x="{x}" y="{y}" width="{w}" height="{h}" rx="10"/>')

def main_panel_rect(svg_h, panel_x=PANEL_X, panel_y=None, panel_w=PANEL_W, subtitle='', subtitle_width=None):
    if panel_y is None:
        if subtitle_width is None:
            subtitle_width = panel_w - 12
        panel_y = panel_top_y(subtitle, subtitle_width)
    return Rect(panel_x, panel_y, panel_w, svg_h - panel_y - PANEL_BOTTOM_MARGIN)

def content_rect(panel_rect):
    return panel_rect.inset(panel_rect.w * CONTENT_PAD_X_RATIO, panel_rect.h * CONTENT_PAD_Y_RATIO)

def fixed_content_rect(panel_rect, pad_x, pad_y):
    return panel_rect.inset(pad_x, pad_y)

def panel_width_for_title(panel_w, title, title_inset_x=TITLE_INSET_X):
    return max(panel_w, title_inset_x + estimate_text_width(title, 9.4))

def svg_width_for_panel(panel_w, outer_pad_x=SVG_OUTER_PAD_X):
    return panel_w + outer_pad_x * 2

def panel_title_x(panel_rect):
    return panel_rect.x + TITLE_INSET_X

def centered_in(rect, content_w, content_h):
    return rect.x + (rect.w - content_w) / 2, rect.y + (rect.h - content_h) / 2

def split_rect(rect, weights, gap_ratio=0.04, axis='x'):
    if not weights:
        return []
    total = rect.w if axis == 'x' else rect.h
    gap = total * gap_ratio
    usable = total - gap * max(0, len(weights) - 1)
    spans = proportional_widths(weights, usable)
    out = []
    cursor = rect.x if axis == 'x' else rect.y
    for span in spans:
        if axis == 'x':
            out.append(Rect(cursor, rect.y, span, rect.h))
        else:
            out.append(Rect(rect.x, cursor, rect.w, span))
        cursor += span + gap
    return out

def min_svg_height_for_panel_content(content_h, pad_y=PANEL_PAD_Y, subtitle='', subtitle_width=None, panel_w=PANEL_W):
    if subtitle_width is None:
        subtitle_width = panel_w - 12
    panel_y = panel_top_y(subtitle, subtitle_width)
    return panel_y + content_h + pad_y * 2 + PANEL_BOTTOM_MARGIN

def content_height_for_svg_height(svg_h):
    panel_h = svg_h - 146
    return panel_h * (1 - 2 * CONTENT_PAD_Y_RATIO)

def svg_height_for_required_content(required_content_h):
    inner_ratio = 1 - 2 * CONTENT_PAD_Y_RATIO
    return 146 + required_content_h / inner_ratio

def svg_height_for_panel_content(content_h, edge_pad_y, subtitle='', subtitle_width=None, panel_w=PANEL_W):
    if subtitle_width is None:
        subtitle_width = panel_w - 12
    panel_y = panel_top_y(subtitle, subtitle_width)
    return panel_y + content_h + edge_pad_y * 2 + PANEL_BOTTOM_MARGIN

def proportional_widths(spans, usable, min_width=0):
    if not spans:
        return []
    total = sum(spans)
    if min_width <= 0 or min_width * len(spans) > usable:
        return [usable * span / total for span in spans]
    widths = [0.0] * len(spans)
    remaining = list(range(len(spans)))
    remaining_usable = usable
    remaining_total = total
    while remaining:
        unit = remaining_usable / remaining_total
        forced = [idx for idx in remaining if spans[idx] * unit < min_width]
        if not forced:
            for idx in remaining:
                widths[idx] = spans[idx] * unit
            break
        if len(forced) * min_width > remaining_usable:
            return [usable * span / total for span in spans]
        for idx in forced:
            widths[idx] = float(min_width)
        remaining_usable -= len(forced) * min_width
        remaining_total -= sum(spans[idx] for idx in forced)
        remaining = [idx for idx in remaining if idx not in forced]
        if remaining_total <= 0:
            break
    return widths

def poly_arrow(a, points):
    points = simplify_orthogonal_points(points)
    emitted = []
    for x, y in points:
        pt = (round(x, 1), round(y, 1))
        if not emitted or pt != emitted[-1]:
            emitted.append(pt)
    points = emitted
    pts = ' '.join(f'{x:.1f},{y:.1f}' for x, y in points)
    a.append(f'  <polyline class="arrow" points="{pts}" fill="none"/>')

def edge_label_metrics(label):
    est_w = max(26, len(label) * 6.4 + 12)
    box_h = 18
    return est_w, box_h

def edge_label_bounds(x, y, label, anchor='middle'):
    est_w, box_h = edge_label_metrics(label)
    if anchor == 'middle':
        rect_x = x - est_w / 2
    elif anchor == 'start':
        rect_x = x - 5
    else:
        rect_x = x - est_w + 5
    rect_y = y - 12
    return Rect(rect_x, rect_y, est_w, box_h)

def edge_label(a, x, y, label, anchor='middle'):
    if not label:
        return
    est_w, box_h = edge_label_metrics(label)
    if anchor == 'middle':
        rect_x = x - est_w / 2
        text_x = x
    elif anchor == 'start':
        rect_x = x - 5
        text_x = x
    else:
        rect_x = x - est_w + 5
        text_x = x
    rect_y = y - 12
    a.append(f'  <rect class="edge-label-bg" x="{rect_x:.1f}" y="{rect_y:.1f}" width="{est_w:.1f}" height="{box_h}" rx="6"/>')
    text(a, text_x, y, label, 'muted', anchor)

def anchor_point(rect, anchor):
    if isinstance(anchor, str) and len(anchor) > 1 and anchor[0] in 'nsew':
        side = anchor[0]
        rest = anchor[1:]
        try:
            t = float(rest)
        except ValueError:
            t = None
        if t is not None:
            t = clamp(t, 0.0, 1.0)
            if side == 'n':
                return (rect.x + rect.w * t, rect.y)
            if side == 's':
                return (rect.x + rect.w * t, rect.bottom)
            if side == 'w':
                return (rect.x, rect.y + rect.h * t)
            if side == 'e':
                return (rect.right, rect.y + rect.h * t)
    anchors = {
        'n': (rect.cx, rect.y),
        's': (rect.cx, rect.bottom),
        'e': (rect.right, rect.cy),
        'w': (rect.x, rect.cy),
        'ne': (rect.right, rect.y),
        'nw': (rect.x, rect.y),
        'se': (rect.right, rect.bottom),
        'sw': (rect.x, rect.bottom),
        'c': (rect.cx, rect.cy),
    }
    return anchors[anchor]

def anchor_stub_point(rect, anchor, stub=STATE_EDGE_STUB):
    x, y = anchor_point(rect, anchor)
    side = anchor[0] if isinstance(anchor, str) and anchor else anchor
    if side == 'n' or anchor in ('ne', 'nw'):
        return (x, y - stub)
    if side == 's' or anchor in ('se', 'sw'):
        return (x, y + stub)
    if side == 'e':
        return (x + stub, y)
    if side == 'w':
        return (x - stub, y)
    return (x, y)

def simplify_orthogonal_points(points):
    if not points:
        return []
    simplified = [points[0]]
    for point in points[1:]:
        if point != simplified[-1]:
            simplified.append(point)
    changed = True
    while changed and len(simplified) >= 3:
        changed = False
        out = [simplified[0]]
        for i in range(1, len(simplified) - 1):
            a = out[-1]
            b = simplified[i]
            c = simplified[i + 1]
            if (a[0] == b[0] == c[0]) or (a[1] == b[1] == c[1]):
                changed = True
                continue
            out.append(b)
        out.append(simplified[-1])
        simplified = out
    return simplified

def orthogonalize_points(points, first_axis='x'):
    if not points:
        return []
    orth = [points[0]]
    axis = first_axis
    for target in points[1:]:
        cur = orth[-1]
        if cur == target:
            continue
        if cur[0] != target[0] and cur[1] != target[1]:
            corner = (target[0], cur[1]) if axis == 'x' else (cur[0], target[1])
            orth.append(corner)
            axis = 'y' if axis == 'x' else 'x'
        orth.append(target)
        if orth[-2][0] == orth[-1][0]:
            axis = 'x'
        elif orth[-2][1] == orth[-1][1]:
            axis = 'y'
    orth = simplify_orthogonal_points(orth)
    for a, b in zip(orth, orth[1:]):
        if a[0] != b[0] and a[1] != b[1]:
            raise ValueError('non-orthogonal state-machine segment')
    return orth

def clamp(value, lo, hi):
    return max(lo, min(hi, value))

def nearest_orthogonal_segment(points, px, py):
    best = None
    for ax, ay, bx, by in ((a[0], a[1], b[0], b[1]) for a, b in zip(points, points[1:])):
        if ax == bx:
            nearest_x = ax
            nearest_y = clamp(py, min(ay, by), max(ay, by))
            dist2 = (px - nearest_x) ** 2 + (py - nearest_y) ** 2
            orientation = 'vertical'
            seg = (ax, ay, bx, by)
        elif ay == by:
            nearest_x = clamp(px, min(ax, bx), max(ax, bx))
            nearest_y = ay
            dist2 = (px - nearest_x) ** 2 + (py - nearest_y) ** 2
            orientation = 'horizontal'
            seg = (ax, ay, bx, by)
        else:
            raise ValueError('expected orthogonal segment')
        if best is None or dist2 < best[0]:
            best = (dist2, orientation, seg)
    return best

def clear_state_label(points, x, y, label, anchor='middle', margin=STATE_LABEL_MARGIN):
    eps = 0.1
    for _ in range(4):
        moved = False
        bounds = edge_label_bounds(x, y, label, anchor)
        for ax, ay, bx, by in ((a[0], a[1], b[0], b[1]) for a, b in zip(points, points[1:])):
            if ay == by:
                line_y = ay
                seg_x0 = min(ax, bx)
                seg_x1 = max(ax, bx)
                if bounds.right + margin < seg_x0 or bounds.x - margin > seg_x1:
                    continue
                if bounds.y - margin <= line_y <= bounds.bottom + margin:
                    above_y = line_y - (bounds.h - (y - bounds.y)) - margin
                    below_y = line_y + (y - bounds.y) + margin
                    new_y = above_y if y <= line_y else below_y
                    if abs(new_y - y) > eps:
                        y = new_y
                        moved = True
                        break
            elif ax == bx:
                line_x = ax
                seg_y0 = min(ay, by)
                seg_y1 = max(ay, by)
                if bounds.bottom + margin < seg_y0 or bounds.y - margin > seg_y1:
                    continue
                if bounds.x - margin <= line_x <= bounds.right + margin:
                    if anchor == 'start':
                        right_dx = bounds.w + 5 + margin
                        left_dx = 5 + margin
                    elif anchor == 'end':
                        right_dx = 5 + margin
                        left_dx = bounds.w - 5 + margin
                    else:
                        right_dx = bounds.w / 2 + margin
                        left_dx = bounds.w / 2 + margin
                    new_x = line_x + right_dx if x >= line_x else line_x - left_dx
                    if abs(new_x - x) > eps:
                        x = new_x
                        moved = True
                        break
        if not moved:
            break
    return x, y

def route_turn_count(route):
    return max(0, len(route) - 2)

def route_total_length(route):
    return sum(abs(b[0] - a[0]) + abs(b[1] - a[1]) for a, b in zip(route, route[1:]))

def rect_intersects_segment(rect, a, b, margin=1.0):
    rx0 = rect.x - margin
    rx1 = rect.right + margin
    ry0 = rect.y - margin
    ry1 = rect.bottom + margin
    ax, ay = a
    bx, by = b
    if ax == bx:
        if not (rx0 < ax < rx1):
            return False
        y0 = min(ay, by)
        y1 = max(ay, by)
        return not (y1 <= ry0 or y0 >= ry1)
    if ay == by:
        if not (ry0 < ay < ry1):
            return False
        x0 = min(ax, bx)
        x1 = max(ax, bx)
        return not (x1 <= rx0 or x0 >= rx1)
    raise ValueError('expected orthogonal segment')

def route_hits_rects(route, rects, margin=1.0):
    for rect in rects:
        for a, b in zip(route, route[1:]):
            if rect_intersects_segment(rect, a, b, margin):
                return True
    return False

def minimal_state_route(points, first_axis, blockers):
    routed = orthogonalize_points(points, first_axis)
    if len(points) > 4:
        return routed
    start_edge = points[0]
    end_edge = points[-1]
    corner = (end_edge[0], start_edge[1]) if first_axis == 'x' else (start_edge[0], end_edge[1])
    direct = simplify_orthogonal_points([start_edge, corner, end_edge])
    if route_hits_rects(direct, blockers, margin=2.0):
        direct_points = [points[0], points[1], points[-2], points[-1]]
        direct = orthogonalize_points(direct_points, first_axis)
        if route_hits_rects(direct, blockers, margin=2.0):
            return routed
    direct_score = (route_turn_count(direct), route_total_length(direct))
    routed_score = (route_turn_count(routed), route_total_length(routed))
    return direct if direct_score <= routed_score else routed

def proportional_widths_with_mins(spans, usable, min_widths):
    if not spans:
        return []
    if len(spans) != len(min_widths):
        raise ValueError('spans and min_widths must have the same length')
    min_total = sum(min_widths)
    if min_total > usable:
        scale = usable / min_total
        return [min_w * scale for min_w in min_widths]
    total = sum(spans)
    if total <= 0:
        extra = (usable - min_total) / len(spans)
        return [min_w + extra for min_w in min_widths]
    extra_usable = usable - min_total
    return [min_w + extra_usable * span / total for min_w, span in zip(min_widths, spans)]

def segmented_bar(a, x, y, total_w, segments, h=58, gap=10, min_width=72, min_widths=None):
    spans = [segment[3] if len(segment) > 3 else 1 for segment in segments]
    usable = total_w - gap * max(0, len(segments) - 1)
    if min_widths is not None:
        widths = proportional_widths_with_mins(spans, usable, min_widths)
    else:
        widths = proportional_widths(spans, usable, min_width=min_width)
    cur_x = x
    boxes = []
    for segment, seg_w in zip(segments, widths):
        label, sub, cls = segment[:3]
        box(a, cur_x, y, seg_w, h, label, cls, sub)
        boxes.append((cur_x, y, seg_w, h))
        cur_x += seg_w + gap
    return boxes

def tree_label_from_route(route, label_clearance, side_label_dx):
    best_h = None
    best_v = None
    for (ax, ay), (bx, by) in zip(route, route[1:]):
        if ay == by:
            span = abs(bx - ax)
            if best_h is None or span > best_h[0]:
                best_h = (span, (ax + bx) / 2, ay)
        elif ax == bx:
            span = abs(by - ay)
            if best_v is None or span > best_v[0]:
                best_v = (span, ax, (ay + by) / 2)
    if best_h is not None:
        _, x, y = best_h
        return x, y - label_clearance, 'middle'
    if best_v is not None:
        _, x, y = best_v
        return x + side_label_dx, y + 4, 'start'
    return route[0][0], route[0][1] - label_clearance, 'middle'

def tree(
    path,
    title,
    subtitle,
    nodes,
    edges,
    w=760,
    h=420,
    panel_x=44,
    panel_y=None,
    panel_w=PANEL_W,
    label_clearance=TREE_LABEL_CLEARANCE,
    edge_clearance=TREE_EDGE_CLEARANCE,
    side_label_dx=TREE_SIDE_LABEL_DX,
    min_vertical_shaft=TREE_MIN_VERTICAL_SHAFT,
    min_vertical_tail=TREE_MIN_VERTICAL_TAIL,
    min_horizontal_shaft=TREE_MIN_HORIZONTAL_SHAFT,
    min_horizontal_tail=TREE_MIN_HORIZONTAL_TAIL,
    relative=False,
):
    panel_w = panel_width_for_title(panel_w, title)
    svg_w = svg_width_for_panel(panel_w)
    subtitle_width = panel_w - 12
    panel_rect = main_panel_rect(h, SVG_OUTER_PAD_X, panel_y, panel_w, subtitle, subtitle_width)
    a = start(title, subtitle, svg_w, h, title_x=panel_title_x(panel_rect), subtitle_x=panel_title_x(panel_rect), subtitle_width=panel_rect.w - 12)
    panel(a, panel_rect.x, panel_rect.y, panel_rect.w, panel_rect.h)
    content = fixed_content_rect(panel_rect, TREE_PANEL_PAD_X, TREE_PANEL_PAD_Y)
    node_map={}
    for node_id, x, y, bw, bh, label, sub, cls in nodes:
        rect = content.child(x, y, bw, bh) if relative else Rect(x, y, bw, bh)
        node_map[node_id] = rect
        box(a, rect.x, rect.y, rect.w, rect.h, label, cls, sub)
    for edge in edges:
        if len(edge) == 3:
            src, dst, label = edge
            src_anchor = None
            dst_anchor = None
        elif len(edge) == 5:
            src, dst, label, src_anchor, dst_anchor = edge
        else:
            raise ValueError('tree edges must be (src, dst, label) or (src, dst, label, src_anchor, dst_anchor)')
        src_rect = node_map[src]
        dst_rect = node_map[dst]
        if src_anchor or dst_anchor:
            if not src_anchor or not dst_anchor:
                raise ValueError(f'tree edge {src}->{dst} must provide both anchors')
            route_start = anchor_stub_point(src_rect, src_anchor, edge_clearance)
            route_end = anchor_stub_point(dst_rect, dst_anchor, edge_clearance)
            first_axis = 'x' if src_anchor in ('e', 'w', 'ne', 'nw', 'se', 'sw') else 'y'
            route = orthogonalize_points([route_start, route_end], first_axis)
            poly_arrow(a, route)
            if label:
                lx, ly, anchor = tree_label_from_route(route, label_clearance, side_label_dx)
                edge_label(a, lx, ly, label, anchor)
            continue
        if dst_rect.y > src_rect.bottom:
            start_x = src_rect.cx
            start_y = src_rect.bottom + edge_clearance
            end_x = dst_rect.cx
            end_y = dst_rect.y - edge_clearance
            branch_y = start_y + min_vertical_shaft
            if end_y - branch_y < min_vertical_tail:
                raise ValueError(f'tree edge {src}->{dst} needs more vertical space')
            poly_arrow(a, [(start_x, start_y), (start_x, branch_y), (end_x, branch_y), (end_x, end_y)])
            if label:
                if abs(end_x - start_x) < 1:
                    edge_label(a, start_x + side_label_dx, (start_y + end_y) / 2 + 4, label, 'start')
                else:
                    edge_label(a, (start_x + end_x) / 2, branch_y - label_clearance, label, 'middle')
        elif dst_rect.cx < src_rect.cx:
            start_x = src_rect.x
            start_y = src_rect.cy
            end_x = dst_rect.right
            end_y = dst_rect.cy
            branch_x = start_x - min_horizontal_shaft
            if branch_x - end_x < min_horizontal_tail:
                raise ValueError(f'tree edge {src}->{dst} needs more horizontal space')
            poly_arrow(a, [(start_x, start_y), (branch_x, start_y), (branch_x, end_y), (end_x, end_y)])
            if label:
                edge_label(a, (start_x + end_x) / 2, min(start_y, end_y) - label_clearance, label, 'middle')
        else:
            start_x = src_rect.right
            start_y = src_rect.cy
            end_x = dst_rect.x
            end_y = dst_rect.cy
            branch_x = start_x + min_horizontal_shaft
            if end_x - branch_x < min_horizontal_tail:
                raise ValueError(f'tree edge {src}->{dst} needs more horizontal space')
            poly_arrow(a, [(start_x, start_y), (branch_x, start_y), (branch_x, end_y), (end_x, end_y)])
            if label:
                edge_label(a, (start_x + end_x) / 2, min(start_y, end_y) - label_clearance, label, 'middle')
    write_svg(path, finish(a))

def state_machine(path, title, subtitle, nodes, transitions, w=760, h=420, panel_w=PANEL_W, relative=False):
    panel_w = panel_width_for_title(panel_w, title)
    h = svg_height_for_panel_content(h - 146, STATE_PANEL_PAD_Y, subtitle, panel_w - 12, panel_w) if h <= 260 else h
    svg_w = svg_width_for_panel(panel_w)
    panel_rect = main_panel_rect(h, SVG_OUTER_PAD_X, None, panel_w, subtitle, panel_w - 12)
    a = start(title, subtitle, svg_w, h, title_x=panel_title_x(panel_rect), subtitle_x=panel_title_x(panel_rect), subtitle_width=panel_rect.w - 12)
    panel(a, panel_rect.x, panel_rect.y, panel_rect.w, panel_rect.h)
    content = fixed_content_rect(panel_rect, STATE_PANEL_PAD_X, STATE_PANEL_PAD_Y)
    node_map = {}
    for node_id, x, y, bw, bh, label, sub, cls in nodes:
        rect = content.child(x, y, bw, bh) if relative else Rect(x, y, bw, bh)
        node_map[node_id] = rect
        box(a, rect.x, rect.y, rect.w, rect.h, label, cls, sub)
    for src, src_anchor, dst, dst_anchor, waypoints, label, label_pos, label_anchor in transitions:
        src_edge = anchor_point(node_map[src], src_anchor)
        src_stub = anchor_stub_point(node_map[src], src_anchor)
        dst_stub = anchor_stub_point(node_map[dst], dst_anchor)
        dst_edge = anchor_point(node_map[dst], dst_anchor)
        pts = [src_edge, src_stub]
        for wx, wy in waypoints:
            pts.append(content.point(wx, wy) if relative else (wx, wy))
        pts.extend([dst_stub, dst_edge])
        first_axis = 'x' if src_anchor in ('e', 'w', 'ne', 'nw', 'se', 'sw') else 'y'
        blockers = [rect for node_id, rect in node_map.items() if node_id not in (src, dst)]
        route = minimal_state_route(pts, first_axis, blockers)
        poly_arrow(a, route)
        if label:
            lx, ly = content.point(label_pos[0], label_pos[1]) if relative else label_pos
            lx, ly = clear_state_label(route, lx, ly, label, label_anchor)
            edge_label(a, lx, ly, label, label_anchor)
    write_svg(path, finish(a))

def timeline(path,title,subtitle,steps,w=760,h=None,preferred_w=180,gap=28):
    count = len(steps)
    min_gap = SEQUENCE_ARROW_START_PAD + SEQUENCE_ARROW_END_PAD + SEQUENCE_MIN_HORIZONTAL_SHAFT
    if count > 1:
        gap = max(gap, min_gap)
    box_w = preferred_w
    box_h = max(box_height(label, sub, box_w) for label, sub, _ in steps)
    row_w = count * box_w + max(0, count - 1) * gap
    panel_w = row_w + SEQUENCE_PANEL_PAD_X * 2
    h = svg_height_for_panel_content(box_h, SEQUENCE_PANEL_PAD_Y, subtitle, panel_w - 12, panel_w)
    panel_w = panel_width_for_title(panel_w, title)
    svg_w = svg_width_for_panel(panel_w)
    panel_rect = main_panel_rect(h, SVG_OUTER_PAD_X, None, panel_w, subtitle, panel_w - 12)
    a = start(title, subtitle, svg_w, h, title_x=panel_title_x(panel_rect), subtitle_x=panel_title_x(panel_rect), subtitle_width=panel_rect.w - 12)
    panel(a, panel_rect.x, panel_rect.y, panel_rect.w, panel_rect.h)
    content = fixed_content_rect(panel_rect, SEQUENCE_PANEL_PAD_X, SEQUENCE_PANEL_PAD_Y)
    x = content.x + (content.w - row_w) / 2
    y = panel_rect.y + SEQUENCE_PANEL_PAD_Y
    for i, (label, sub, cls) in enumerate(steps):
        xx = x + i * (box_w + gap)
        box(a, xx, y, box_w, box_h, label, cls, sub)
        if i < count - 1:
            arrow(
                a,
                xx + box_w + SEQUENCE_ARROW_START_PAD,
                y + box_h / 2,
                xx + box_w + gap - SEQUENCE_ARROW_END_PAD,
                y + box_h / 2,
            )
    write_svg(path, finish(a))

def compare_timelines(path, title, subtitle, top_label, top_steps, bottom_label, bottom_steps, w=760, h=None, preferred_w=118, gap=18, row_gap=34):
    count = max(len(top_steps), len(bottom_steps))
    label_gap = 18
    label_w = max(len(top_label), len(bottom_label)) * 8.5
    min_gap = SEQUENCE_ARROW_START_PAD + SEQUENCE_ARROW_END_PAD + SEQUENCE_MIN_HORIZONTAL_SHAFT
    if count > 1:
        gap = max(gap, min_gap)
    box_w = preferred_w
    box_h = max(
        max(box_height(label, sub, box_w) for label, sub, _ in top_steps),
        max(box_height(label, sub, box_w) for label, sub, _ in bottom_steps),
    )
    row_h = box_h
    content_h = row_h * 2 + row_gap + 24
    row_content_w_top = len(top_steps) * box_w + max(0, len(top_steps) - 1) * gap
    row_content_w_bottom = len(bottom_steps) * box_w + max(0, len(bottom_steps) - 1) * gap
    row_content_w = max(row_content_w_top, row_content_w_bottom)
    body_w = label_w + label_gap + row_content_w
    panel_w = body_w + SEQUENCE_PANEL_PAD_X * 2
    h = svg_height_for_panel_content(content_h, SEQUENCE_PANEL_PAD_Y, subtitle, panel_w - 12, panel_w)
    panel_w = panel_width_for_title(panel_w, title)
    svg_w = svg_width_for_panel(panel_w)
    panel_rect = main_panel_rect(h, SVG_OUTER_PAD_X, None, panel_w, subtitle, panel_w - 12)
    a = start(title, subtitle, svg_w, h, title_x=panel_title_x(panel_rect), subtitle_x=panel_title_x(panel_rect), subtitle_width=panel_rect.w - 12)
    panel(a, panel_rect.x, panel_rect.y, panel_rect.w, panel_rect.h)
    content = fixed_content_rect(panel_rect, SEQUENCE_PANEL_PAD_X, SEQUENCE_PANEL_PAD_Y)
    x = content.x + (content.w - body_w) / 2
    y = panel_rect.y + SEQUENCE_PANEL_PAD_Y

    def draw_row(row_y, label, steps):
        text(a, x, row_y + row_h / 2 + 4, label, 'label', 'start')
        start_x = x + label_w + label_gap
        for i, (step_label, sub, cls) in enumerate(steps):
            xx = start_x + i * (box_w + gap)
            box(a, xx, row_y, box_w, row_h, step_label, cls, sub)
            if i < len(steps) - 1:
                arrow(
                    a,
                    xx + box_w + SEQUENCE_ARROW_START_PAD,
                    row_y + row_h / 2,
                    xx + box_w + gap - SEQUENCE_ARROW_END_PAD,
                    row_y + row_h / 2,
                )

    draw_row(y, top_label, top_steps)
    draw_row(y + row_h + row_gap, bottom_label, bottom_steps)
    write_svg(path, finish(a))

def bitfield(path,title,subtitle,bits,notes=None,w=760,h=250,min_box_w=0):
    # Render as a table: each field becomes a row, no proportional sizing.
    if notes:
        subtitle = subtitle.rstrip('.') + '. ' + ' '.join(notes)
    has_subs = any(sub for _,_,sub in bits)
    if has_subs:
        headers = ['Field','Bits','Notes']
        rows = [(lab, str(span), sub) for lab,span,sub in bits]
    else:
        headers = ['Field','Bits']
        rows = [(lab, str(span)) for lab,span,_ in bits]
    table(path,title,subtitle,headers,rows,w=w,h=h)

def flow(path,title,subtitle,steps,w=760,h=None):
    if any('?' in label for label, _, _ in steps):
        raise ValueError(f'flow() should not be used for decision questions: {path}')
    min_gap = SEQUENCE_ARROW_START_PAD + SEQUENCE_ARROW_END_PAD + SEQUENCE_MIN_HORIZONTAL_SHAFT
    max_content_w = PANEL_W - SEQUENCE_PANEL_PAD_X * 2
    use_vertical = len(steps) > 4
    if not use_vertical and len(steps) > 1:
        max_horizontal_bw = (max_content_w - min_gap * (len(steps) - 1)) / len(steps)
        use_vertical = max_horizontal_bw < 96
    widths = None
    if not use_vertical:
        preferred_bw = min(max_content_w * 0.22, (max_content_w - min_gap * max(0, len(steps) - 1)) / max(1, len(steps)))
        widths = [landscape_box_dims(lab, sub, min_w=96, preferred_w=preferred_bw)[0] for lab, sub, _ in steps]
        min_row_w = sum(widths) + max(0, len(steps) - 1) * min_gap
        use_vertical = min_row_w > max_content_w
    if use_vertical:
        preferred_box_w = max_content_w * 0.42
        box_w = max(landscape_box_dims(lab, sub, min_w=96, preferred_w=preferred_box_w)[0] for lab, sub, _ in steps)
        min_vertical_gap = SEQUENCE_ARROW_START_PAD + SEQUENCE_ARROW_END_PAD + SEQUENCE_MIN_VERTICAL_SHAFT
        gap_between = max(18, min_vertical_gap)
        box_heights = [box_height(lab, sub, box_w) for lab, sub, _ in steps]
        content_h = sum(box_heights) + gap_between * (len(steps) - 1)
        panel_w = min(PANEL_W, box_w + SEQUENCE_PANEL_PAD_X * 2)
        h = svg_height_for_panel_content(content_h, SEQUENCE_PANEL_PAD_Y, subtitle, panel_w - 12, panel_w)
    else:
        box_h = max(78, max(box_height(lab, sub, bw) for (lab, sub, _), bw in zip(steps, widths)))
        gap = (max_content_w - sum(widths)) / max(1, len(steps) - 1) if len(steps) > 1 else 0
        panel_w = min(PANEL_W, sum(widths) + max(0, len(steps) - 1) * gap + SEQUENCE_PANEL_PAD_X * 2)
        h = svg_height_for_panel_content(box_h, SEQUENCE_PANEL_PAD_Y, subtitle, panel_w - 12, panel_w)
    panel_w = panel_width_for_title(panel_w, title)
    svg_w = svg_width_for_panel(panel_w)
    panel_rect = main_panel_rect(h, SVG_OUTER_PAD_X, None, panel_w, subtitle, panel_w - 12)
    a = start(title, subtitle, svg_w, h, title_x=panel_title_x(panel_rect), subtitle_x=panel_title_x(panel_rect), subtitle_width=panel_rect.w - 12)
    panel(a, panel_rect.x, panel_rect.y, panel_rect.w, panel_rect.h)
    content = fixed_content_rect(panel_rect, SEQUENCE_PANEL_PAD_X, SEQUENCE_PANEL_PAD_Y)
    if not use_vertical:
        row_w = sum(widths) + max(0, len(steps) - 1) * gap
        x = content.x + (content.w - row_w) / 2
        y = panel_rect.y + SEQUENCE_PANEL_PAD_Y
        for i,(lab,sub,cls) in enumerate(steps):
            bw = widths[i]
            xx = x
            box(a, xx, y, bw, box_h, lab, cls, sub)
            if i < len(steps)-1:
                next_x = xx + bw + gap
                arrow(
                    a,
                    xx + bw + SEQUENCE_ARROW_START_PAD,
                    y + box_h / 2,
                    next_x - SEQUENCE_ARROW_END_PAD,
                    y + box_h / 2,
                )
            x = xx + bw + gap
    else:
        x = content.x + (content.w - box_w) / 2
        y = panel_rect.y + SEQUENCE_PANEL_PAD_Y
        center_x = x + box_w / 2
        cur_y = y
        for i,(lab,sub,cls) in enumerate(steps):
            bh = box_heights[i]
            box(a,x,cur_y,box_w,bh,lab,cls,sub)
            if i < len(steps)-1:
                next_y = cur_y + bh + gap_between
                if next_y - (cur_y + bh) - (SEQUENCE_ARROW_START_PAD + SEQUENCE_ARROW_END_PAD) < SEQUENCE_MIN_VERTICAL_SHAFT:
                    raise ValueError(f'flow {path} needs more vertical arrow space')
                arrow(
                    a,
                    center_x,
                    cur_y + bh + SEQUENCE_ARROW_START_PAD,
                    center_x,
                    next_y - SEQUENCE_ARROW_END_PAD,
                )
            cur_y += bh + gap_between
    write_svg(path, finish(a))

def stack(path,title,subtitle,items,w=760,h=None,top_note=None,bottom_note=None):
    bw = min(max((landscape_box_dims(lab, sub, min_w=170)[0] for lab, sub, _ in items), default=220), PANEL_W - STACK_EDGE_PAD_X * 2)
    row_heights = [max(56, box_height(lab, sub, bw)) for lab, sub, _ in items]
    fixed_row_h = sum(row_heights)
    gap_count = max(0, len(items) - 1)
    note_top_h = STACK_NOTE_H if top_note else 0
    note_bottom_h = STACK_NOTE_H if bottom_note else 0
    content_h = fixed_row_h + gap_count * STACK_ROW_GAP + note_top_h + note_bottom_h
    panel_w = bw + STACK_EDGE_PAD_X * 2
    panel_h = content_h + STACK_EDGE_PAD_Y * 2
    h = max(min_svg_height_for_panel_content(200, subtitle=subtitle, subtitle_width=panel_w - 12, panel_w=panel_w), panel_top_y(subtitle, panel_w - 12) + panel_h + PANEL_BOTTOM_MARGIN)
    panel_w = panel_width_for_title(panel_w, title)
    svg_w = svg_width_for_panel(panel_w)
    panel_rect = main_panel_rect(h, SVG_OUTER_PAD_X, None, panel_w, subtitle, panel_w - 12)
    a = start(title, subtitle, svg_w, h, title_x=panel_title_x(panel_rect), subtitle_x=panel_title_x(panel_rect), subtitle_width=panel_rect.w - 12)
    panel(a, panel_rect.x, panel_rect.y, panel_rect.w, panel_rect.h)
    x = panel_rect.x + (panel_rect.w - bw) / 2
    cur_y = panel_rect.y + STACK_EDGE_PAD_Y
    if top_note:
        text(a, x + bw/2, cur_y + 14, top_note, 'muted', 'middle')
        cur_y += note_top_h
    for i,(lab,sub,cls) in enumerate(items):
        box(a, x, cur_y, bw, row_heights[i], lab, cls, sub, mono=False)
        cur_y += row_heights[i] + STACK_ROW_GAP
    if items:
        cur_y -= STACK_ROW_GAP
    if bottom_note:
        text(a, x + bw/2, cur_y + 16, bottom_note, 'muted', 'middle')
    write_svg(path, finish(a))

def table(path,title,subtitle,headers,rows,w=760,h=None):
    header_h = 38
    weights = []
    for idx, head in enumerate(headers):
        cell_texts = [head, *(row[idx] for row in rows)]
        longest = max(len(str(cell)) for cell in cell_texts)
        weights.append(max(1, longest))
    natural_widths = []
    for idx, head in enumerate(headers):
        cell_texts = [head, *(row[idx] for row in rows)]
        widest = max(
            estimate_text_width(str(cell), 7.4 if ('0x' in str(cell) or str(cell).isdigit()) else 7.0)
            for cell in cell_texts
        )
        natural_widths.append(max(82, widest + 26))
    total = sum(natural_widths)
    max_total = PANEL_W - TABLE_PANEL_PAD_X * 2
    if total > max_total:
        widths = proportional_widths(weights, max_total, min_width=max_total / (len(headers) * 2.4))
        total = max_total
    else:
        widths = natural_widths
    header_lines = [lines(head, max(8, int(widths[i] / 9))) for i, head in enumerate(headers)]
    header_h = max(38, max(len(x) for x in header_lines) * 16 + 14)
    wrapped_rows = []
    row_heights = []
    for row in rows:
        wrapped = []
        max_lines = 1
        for i, cell in enumerate(row):
            cell_lines = lines(cell, max(8, int(widths[i] / 9)))
            wrapped.append(cell_lines)
            max_lines = max(max_lines, len(cell_lines))
        wrapped_rows.append(wrapped)
        row_heights.append(max(38, max_lines * 16 + 14))
    table_h = header_h + sum(row_heights)
    panel_w = total + TABLE_PANEL_PAD_X * 2
    h = svg_height_for_panel_content(table_h, TABLE_PANEL_PAD_Y, subtitle, panel_w - 12, panel_w)
    panel_w = panel_width_for_title(panel_w, title)
    svg_w = svg_width_for_panel(panel_w)
    panel_rect = main_panel_rect(h, SVG_OUTER_PAD_X, None, panel_w, subtitle, panel_w - 12)
    a = start(title, subtitle, svg_w, h, title_x=panel_title_x(panel_rect), subtitle_x=panel_title_x(panel_rect), subtitle_width=panel_rect.w - 12)
    panel(a, panel_rect.x, panel_rect.y, panel_rect.w, panel_rect.h)
    x = panel_rect.x + TABLE_PANEL_PAD_X
    y = panel_rect.y + TABLE_PANEL_PAD_Y
    a.append(f'  <rect class="table-shell" x="{x}" y="{y}" width="{total}" height="{table_h}" rx="8"/>')
    a.append(f'  <rect class="table-headband" x="{x}" y="{y}" width="{total}" height="{header_h}" rx="8"/>')
    a.append(f'  <line class="table-rule" x1="{x}" y1="{y+header_h}" x2="{x+total}" y2="{y+header_h}"/>')
    cur=x
    for i, (head,bw,head_ln) in enumerate(zip(headers,widths,header_lines)):
        cy = y + header_h / 2 - (len(head_ln) - 1) * 16 / 2 + 5
        for ln in head_ln:
            text(a, cur + bw/2, cy, ln, 'label', 'middle')
            cy += 16
        cur += bw
        if i < len(headers) - 1:
            a.append(f'  <line class="table-rule" x1="{cur}" y1="{y}" x2="{cur}" y2="{y+table_h}"/>')
    row_y = y + header_h
    for r,row in enumerate(rows):
        row_h = row_heights[r]
        if r < len(rows) - 1:
            a.append(f'  <line class="table-rule" x1="{x}" y1="{row_y+row_h}" x2="{x+total}" y2="{row_y+row_h}"/>')
        cur=x
        for c,cell in enumerate(row):
            klass = 'mono' if ('0x' in str(cell) or str(cell).isdigit()) else 'small'
            cell_lines = wrapped_rows[r][c]
            cy = row_y + row_h / 2 - (len(cell_lines) - 1) * 16 / 2 + 5
            for ln in cell_lines:
                text(a, cur + widths[c]/2, cy, ln, klass, 'middle')
                cy += 16
            cur += widths[c]
        row_y += row_h
    write_svg(path, finish(a))

def grid_snapshot(path, title, subtitle, headers, row_specs, sample_cells, notes=None, w=760, h=410):
    grid_h = 204
    note_gap = 18 if notes else 0
    note_line_gap = 18
    notes_h = (len(notes) - 1) * note_line_gap + 14 if notes else 0
    addr_w = max(estimate_text_width(headers[0], 6.8), max((estimate_text_width(addr, 7.2) for addr, _, _ in row_specs), default=0)) + 24
    row_w = max(estimate_text_width(headers[1], 6.8), max((estimate_text_width(row_label, 7.0) for _, row_label, _ in row_specs), default=0)) + 24
    sample_w = sum(max(48, estimate_text_width(value or '...', 6.8) + 16) for _, _, value in sample_cells)
    sample_w += max(0, len(sample_cells) - 1) * 10
    data_w = max(sample_w, estimate_text_width(headers[2], 6.8) + estimate_text_width(headers[3], 6.8) + 100)
    content_w = addr_w + row_w + data_w + 32
    panel_w = min(PANEL_W, content_w + SNAPSHOT_PANEL_PAD_X * 2)
    content_h = grid_h + note_gap + notes_h
    h = max(h, svg_height_for_panel_content(content_h, SNAPSHOT_PANEL_PAD_Y, subtitle, panel_w - 12, panel_w))
    panel_w = panel_width_for_title(panel_w, title)
    svg_w = svg_width_for_panel(panel_w)
    a = start(title, subtitle, svg_w, h, title_x=SVG_OUTER_PAD_X + TITLE_INSET_X, subtitle_x=SVG_OUTER_PAD_X + TITLE_INSET_X, subtitle_width=panel_w - 12)
    panel_rect = main_panel_rect(h, SVG_OUTER_PAD_X, None, panel_w, subtitle, panel_w - 12)
    panel(a, panel_rect.x, panel_rect.y, panel_rect.w, panel_rect.h)
    content = fixed_content_rect(panel_rect, SNAPSHOT_PANEL_PAD_X, SNAPSHOT_PANEL_PAD_Y)
    grid = Rect(content.x, content.y, content.w, grid_h)
    addr_col, row_col, data_col = split_rect(grid, [1.3, 1.1, 4.6], gap_ratio=0.05)
    header_y = grid.y + grid.h * 0.05
    text(a, addr_col.x, header_y, headers[0], 'colhead')
    text(a, row_col.x, header_y, headers[1], 'colhead')
    text(a, data_col.x + data_col.w * 0.14, header_y, headers[2], 'colhead')
    text(a, data_col.right, header_y, headers[3], 'colhead', 'end')

    row_area = Rect(grid.x, grid.y + grid.h * 0.12, grid.w, grid.h * 0.82)
    row_frames = split_rect(row_area, [1, 1, 1, 1], gap_ratio=0.08, axis='y')
    byte_strip = Rect(data_col.x, row_frames[0].y + row_frames[0].h * 0.16, data_col.w, row_frames[0].h * 0.58)
    spans = [cell[0] for cell in sample_cells]
    data_boxes = split_rect(byte_strip, spans, gap_ratio=0.02)
    for rect, (_, cls, value) in zip(data_boxes, sample_cells):
        a.append(f'  <rect class="{cls}" x="{rect.x:.1f}" y="{rect.y:.1f}" width="{rect.w:.1f}" height="{rect.h:.1f}" rx="6"/>')
        if value:
            a.append(f'  <text class="byte" x="{rect.cx:.1f}" y="{rect.cy:.1f}">{esc(value)}</text>')
    for rect, (_, cls, value) in zip(data_boxes, sample_cells):
        if cls == 'gap':
            text(a, rect.cx, rect.cy + 4, '...', 'muted', 'middle')

    for idx, frame in enumerate(row_frames):
        addr, row_label, data_label = row_specs[idx]
        text(a, addr_col.x, frame.cy + 4, addr, 'mono')
        text(a, row_col.x, frame.cy + 4, row_label)
        if idx == 0:
            continue
        band = frame.child((data_col.x - grid.x) / grid.w, 0.20, data_col.w / grid.w, 0.44)
        a.append(f'  <rect class="row" x="{band.x:.1f}" y="{band.y:.1f}" width="{band.w:.1f}" height="{band.h:.1f}" rx="6"/>')
        if data_label:
            text(a, band.cx, band.cy + 4, data_label, 'muted', 'middle')
    text(a, addr_col.x + addr_col.w * 0.25, row_frames[2].bottom + row_frames[2].h * 0.18, '...', 'muted', 'middle')
    text(a, data_col.x + data_col.w * 0.46, row_frames[2].bottom + row_frames[2].h * 0.18, '...', 'muted', 'middle')

    if notes:
        notes_rect = Rect(content.x, grid.bottom + note_gap, content.w, notes_h)
        note_y = notes_rect.y + 12
        for idx, line in enumerate(notes):
            text(a, notes_rect.x, note_y + idx * note_line_gap, line, 'muted')
    write_svg(path, finish(a))

def tagged_transcript(path, title, subtitle, rows, h=310, headers=('tag', 'message')):
    header_h = 16
    header_gap = 12
    row_h = 22
    row_gap = 10
    row_count = max(len(rows), 1)
    body_h = row_count * row_h + max(0, row_count - 1) * row_gap
    max_tag_len = max((len(tag) for tag, _ in rows), default=0)
    max_msg_len = max((len(msg) for _, msg in rows), default=0)
    tag_width = max(len(headers[0]) * 8 + 10, max_tag_len * 9 + 18)
    msg_width = max(len(headers[1]) * 8 + 20, max_msg_len * 7.0 + 18)
    body_w = tag_width + TRANSCRIPT_COL_GAP + msg_width
    panel_w = min(PANEL_W, max(TRANSCRIPT_MIN_PANEL_W, body_w + TRANSCRIPT_EDGE_PAD_X * 2))
    panel_w = panel_width_for_title(panel_w, title)
    svg_w = svg_width_for_panel(panel_w)
    h = max(h, svg_height_for_panel_content(header_h + header_gap + body_h, TRANSCRIPT_EDGE_PAD_Y, subtitle, panel_w - 12, panel_w))
    panel_rect = main_panel_rect(h, SVG_OUTER_PAD_X, None, panel_w, subtitle, panel_w - 12)
    a = start(title, subtitle, svg_w, h, title_x=panel_title_x(panel_rect), subtitle_x=panel_title_x(panel_rect), subtitle_width=panel_rect.w - 12)
    panel(a, panel_rect.x, panel_rect.y, panel_rect.w, panel_rect.h)
    content = fixed_content_rect(panel_rect, TRANSCRIPT_EDGE_PAD_X, TRANSCRIPT_EDGE_PAD_Y)
    body_rect = Rect(content.x, content.y + header_h + header_gap, content.w, content.h - header_h - header_gap)
    header_y = content.y + 4
    text(a, body_rect.x, header_y, headers[0], 'colhead')
    text(a, body_rect.x + tag_width + TRANSCRIPT_COL_GAP, header_y, headers[1], 'colhead')
    rows_h = len(rows) * row_h + max(0, len(rows) - 1) * row_gap
    row_y = body_rect.y + (body_rect.h - rows_h) / 2
    for tag, msg in rows:
        frame = Rect(body_rect.x, row_y, body_rect.w, row_h)
        tag_rect = Rect(frame.x, frame.y + (frame.h - 18) / 2, tag_width, 18)
        a.append(f'  <rect class="tag" x="{tag_rect.x:.1f}" y="{tag_rect.y:.1f}" width="{tag_rect.w:.1f}" height="{tag_rect.h:.1f}" rx="6"/>')
        text(a, tag_rect.cx, tag_rect.cy + 4, tag, 'small', 'middle')
        text(a, tag_rect.right + TRANSCRIPT_COL_GAP, frame.cy + 4, msg, 'small')
        row_y += row_h + row_gap
    write_svg(path, finish(a))

def buffer_transfer(path, title, subtitle, left_title, right_title, source_labels, dest_label, dest_sub, w=760):
    edge_pad_y = 24
    header_h = 18
    header_gap = 14
    source_frame_w = max(max((estimate_text_width(label, 7.0) for label in source_labels), default=0) + 24, 56)
    left_gap = 12
    buffer_w, dest_h = landscape_box_dims(dest_label, dest_sub, min_w=132)
    body_h = max(56, dest_h)
    source_frame_w = max(source_frame_w, body_h)
    left_w = len(source_labels) * source_frame_w + max(0, len(source_labels) - 1) * left_gap
    right_w = buffer_w / 0.68
    body_w = left_w + 56 + right_w
    panel_w = body_w + TRANSFER_PANEL_PAD_X * 2
    h = svg_height_for_panel_content(header_h + header_gap + body_h, edge_pad_y, subtitle, panel_w - 12, panel_w)
    panel_w = panel_width_for_title(panel_w, title)
    svg_w = svg_width_for_panel(panel_w)
    panel_rect = main_panel_rect(h, SVG_OUTER_PAD_X, None, panel_w, subtitle, panel_w - 12)
    a = start(title, subtitle, svg_w, h, title_x=panel_title_x(panel_rect), subtitle_x=panel_title_x(panel_rect), subtitle_width=panel_rect.w - 12)
    panel(a, panel_rect.x, panel_rect.y, panel_rect.w, panel_rect.h)
    content = fixed_content_rect(panel_rect, TRANSFER_PANEL_PAD_X, edge_pad_y)
    body_y = content.y + header_h + header_gap
    body_rect = Rect(content.x, body_y, body_w, body_h)
    inter_gap = 56
    left_rect = Rect(body_rect.x, body_rect.y, left_w, body_rect.h)
    right_rect = Rect(left_rect.right + inter_gap, body_rect.y, right_w, body_rect.h)
    text(a, left_rect.x, content.y + 4, left_title, 'colhead')
    text(a, right_rect.cx, content.y + 4, right_title, 'colhead', 'middle')
    sector_frames = []
    cur_x = left_rect.x
    for _ in source_labels:
        sector_frames.append(Rect(cur_x, left_rect.y, source_frame_w, left_rect.h))
        cur_x += source_frame_w + left_gap
    for frame, label in zip(sector_frames, source_labels):
        a.append(f'  <rect class="blue" x="{frame.x:.1f}" y="{frame.y:.1f}" width="{frame.w:.1f}" height="{frame.h:.1f}" rx="6"/>')
        text(a, frame.cx, frame.cy + 4, label, 'small', 'middle')
    buffer_rect = right_rect.child(0.16, 0.00, 0.68, 1.00)
    arrow(a, left_rect.right + body_rect.w * 0.02, left_rect.cy, buffer_rect.x - body_rect.w * 0.03, buffer_rect.cy)
    a.append(f'  <rect class="green" x="{buffer_rect.x:.1f}" y="{buffer_rect.y:.1f}" width="{buffer_rect.w:.1f}" height="{buffer_rect.h:.1f}" rx="8"/>')
    text(a, buffer_rect.cx, buffer_rect.y + buffer_rect.h * 0.34, dest_label, 'label', 'middle')
    text(a, buffer_rect.cx, buffer_rect.y + buffer_rect.h * 0.68, dest_sub, 'small', 'middle')
    write_svg(path, finish(a))

def lane_walk(path, title, subtitle, lanes, w=760, h=360):
    caption_w = max((estimate_text_width(caption, 7.2) for caption, _ in lanes), default=0) + 20
    node_w = max((landscape_box_dims(label, min_w=96)[0] for _, nodes in lanes for label, _ in nodes), default=110)
    node_h = max((landscape_box_dims(label, min_w=96, preferred_w=node_w)[1] for _, nodes in lanes for label, _ in nodes), default=56)
    path_gap = 18
    path_w = max((len(nodes) * node_w + max(0, len(nodes) - 1) * path_gap for _, nodes in lanes), default=node_w)
    content_w = caption_w + 22 + path_w
    panel_w = content_w + LANE_PANEL_PAD_X * 2
    lane_count = max(len(lanes), 1)
    lane_h = max(62, node_h + 18)
    content_h = lane_count * lane_h + max(0, lane_count - 1) * 20
    h = max(h, svg_height_for_panel_content(content_h, LANE_PANEL_PAD_Y, subtitle, panel_w - 12, panel_w))
    panel_w = panel_width_for_title(panel_w, title)
    svg_w = svg_width_for_panel(panel_w)
    a = start(title, subtitle, svg_w, h, title_x=SVG_OUTER_PAD_X + TITLE_INSET_X, subtitle_x=SVG_OUTER_PAD_X + TITLE_INSET_X, subtitle_width=panel_w - 12)
    panel_rect = main_panel_rect(h, SVG_OUTER_PAD_X, None, panel_w, subtitle, panel_w - 12)
    panel(a, panel_rect.x, panel_rect.y, panel_rect.w, panel_rect.h)
    content = fixed_content_rect(panel_rect, LANE_PANEL_PAD_X, LANE_PANEL_PAD_Y)
    row_frames = split_rect(content, [1] * len(lanes), gap_ratio=0.10, axis='y')
    for frame, (caption, nodes) in zip(row_frames, lanes):
        panel(a, frame.x, frame.y, frame.w, frame.h)
        inner = frame.inset(frame.w * 0.04, frame.h * 0.18)
        caption_rect, path_rect = split_rect(inner, [1.5, 4.5], gap_ratio=0.06)
        text(a, caption_rect.x, caption_rect.cy + 4, caption, 'label')
        node_rects = split_rect(path_rect, [1] * len(nodes), gap_ratio=0.12)
        box_rects = []
        for rect, (label, cls) in zip(node_rects, nodes):
            inner_box = rect.child(0.08, 0.08, 0.84, 0.84)
            box(a, inner_box.x, inner_box.y, inner_box.w, inner_box.h, label, cls)
            box_rects.append(inner_box)
        for left, right in zip(box_rects, box_rects[1:]):
            arrow(a, left.right + path_rect.w * 0.02, left.cy, right.x - path_rect.w * 0.02, right.cy)
    write_svg(path, finish(a))

def split_overview(path, title, subtitle, top_title, top_segments, bottom_title, bottom_rows, w=760, h=360):
    section_gap = 22
    top_inner_pad_x = 18
    top_inner_pad_y = 16
    bottom_inner_pad_x = 18
    bottom_inner_pad_y = 16
    title_row_h = 16
    title_to_content_gap = 10
    caption_gap = 18
    muted_line_h = 16
    bottom_row_gap = 8
    bottom_row_entries = []
    for row in bottom_rows:
        name, desc = row[:2]
        if len(row) > 2:
            name_cls = row[2]
        else:
            name_cls = 'small' if str(name).startswith('/') else 'label'
        bottom_row_entries.append((name, desc, name_cls))
    top_segment_widths = [landscape_box_dims(label, '', min_w=88)[0] for label, _, _, _ in top_segments]
    top_bar_gap = 8
    top_bar_w = sum(top_segment_widths) + max(0, len(top_segments) - 1) * top_bar_gap
    top_bar_h = max(box_height(label, '', width) for (label, _, _, _), width in zip(top_segments, top_segment_widths))
    top_caption_h = max((len(lines(caption, 16)) for _, caption, _, _ in top_segments), default=1) * muted_line_h
    bottom_name_w = max((estimate_text_width(name, 7.2) for name, _, _ in bottom_row_entries), default=0) + 20
    bottom_desc_w = max((estimate_text_width(desc, 6.8) for _, desc, _ in bottom_row_entries), default=0) + 20
    bottom_w = bottom_name_w + 24 + bottom_desc_w
    content_w = max(top_bar_w, bottom_w)
    panel_w = content_w + SPLIT_PANEL_PAD_X * 2
    bottom_row_h = max(
        max(len(lines(name, max(8, int(bottom_name_w / 9)))) for name, _, _ in bottom_row_entries) * muted_line_h,
        max(len(lines(desc, max(8, int(bottom_desc_w / 8)))) for _, desc, _ in bottom_row_entries) * muted_line_h,
        18,
    )
    top_panel_h = top_inner_pad_y * 2 + title_row_h + title_to_content_gap + top_bar_h + caption_gap + top_caption_h
    bottom_rows_h = len(bottom_rows) * bottom_row_h + max(0, len(bottom_rows) - 1) * bottom_row_gap
    bottom_panel_h = bottom_inner_pad_y * 2 + title_row_h + title_to_content_gap + bottom_rows_h
    panel_w = panel_width_for_title(panel_w, title)
    top_y = panel_top_y(subtitle, panel_w - 12)
    content_h = top_panel_h + section_gap + bottom_panel_h
    h = max(h, top_y + content_h + PANEL_BOTTOM_MARGIN)
    svg_w = svg_width_for_panel(panel_w)
    a = start(title, subtitle, svg_w, h, title_x=SVG_OUTER_PAD_X + TITLE_INSET_X, subtitle_x=SVG_OUTER_PAD_X + TITLE_INSET_X, subtitle_width=panel_w - 12)
    top_panel = Rect(SVG_OUTER_PAD_X, top_y, panel_w, top_panel_h)
    bottom_panel = Rect(SVG_OUTER_PAD_X, top_panel.bottom + section_gap, panel_w, bottom_panel_h)
    panel(a, top_panel.x, top_panel.y, top_panel.w, top_panel.h)
    panel(a, bottom_panel.x, bottom_panel.y, bottom_panel.w, bottom_panel.h)

    top_inner = fixed_content_rect(top_panel, top_inner_pad_x, top_inner_pad_y)
    top_label_y = top_inner.y + 4
    text(a, top_inner.x, top_label_y, top_title, 'small')
    bar_rect = Rect(top_inner.x, top_inner.y + title_row_h + title_to_content_gap, top_inner.w, top_bar_h)
    segment_boxes = segmented_bar(
        a,
        bar_rect.x,
        bar_rect.y,
        bar_rect.w,
        [(label, '', cls, span) for label, _, cls, span in top_segments],
        h=bar_rect.h,
        gap=top_bar_gap,
        min_widths=top_segment_widths,
    )
    for (x, y, seg_w, seg_h), (_, label, _, _) in zip(segment_boxes, top_segments):
        text(a, x + seg_w / 2, y + seg_h + caption_gap, label, 'muted', 'middle')

    bottom_inner = fixed_content_rect(bottom_panel, bottom_inner_pad_x, bottom_inner_pad_y)
    text(a, bottom_inner.x, bottom_inner.y + 4, bottom_title, 'small')
    rows_rect = Rect(
        bottom_inner.x,
        bottom_inner.y + title_row_h + title_to_content_gap,
        bottom_inner.w,
        bottom_inner.h - title_row_h - title_to_content_gap,
    )
    cur_y = rows_rect.y
    for name, desc, name_cls in bottom_row_entries:
        frame = Rect(rows_rect.x, cur_y, rows_rect.w, bottom_row_h)
        name_rect, desc_rect = split_rect(frame, [1.1, 4.9], gap_ratio=0.06)
        text(a, name_rect.x, name_rect.cy + 4, name, name_cls)
        text(a, desc_rect.x, desc_rect.cy + 4, desc, 'muted')
        cur_y += bottom_row_h + bottom_row_gap
    write_svg(path, finish(a))

def compare_segments(path, title, subtitle, top_label, top_segments, bottom_label, bottom_segments, w=760, h=None, row_gap=42, label_w=92, gap=10, bar_h=58):
    top_min_widths = [landscape_box_dims(label, sub, min_w=72, preferred_w=150)[0] for label, sub, *_ in top_segments]
    bottom_min_widths = [landscape_box_dims(label, sub, min_w=72, preferred_w=150)[0] for label, sub, *_ in bottom_segments]
    top_h = max(bar_h, max(box_height(label, sub, width) for (label, sub, *_), width in zip(top_segments, top_min_widths)))
    bottom_h = max(bar_h, max(box_height(label, sub, width) for (label, sub, *_), width in zip(bottom_segments, bottom_min_widths)))
    content_h = top_h + bottom_h + row_gap
    label_gap = 18
    top_bar_w = sum(top_min_widths) + max(0, len(top_segments) - 1) * gap
    bottom_bar_w = sum(bottom_min_widths) + max(0, len(bottom_segments) - 1) * gap
    label_w = min(max(label_w, estimate_text_width(top_label, 7.2) + 14, estimate_text_width(bottom_label, 7.2) + 14), 140)
    bar_w = max(top_bar_w, bottom_bar_w)
    panel_w = label_w + label_gap + bar_w + COMPARISON_PANEL_PAD_X * 2
    h = svg_height_for_panel_content(content_h, COMPARISON_PANEL_PAD_Y, subtitle, panel_w - 12, panel_w)
    panel_w = panel_width_for_title(panel_w, title)
    svg_w = svg_width_for_panel(panel_w)
    panel_rect = main_panel_rect(h, SVG_OUTER_PAD_X, None, panel_w, subtitle, panel_w - 12)
    a = start(title, subtitle, svg_w, h, title_x=panel_title_x(panel_rect), subtitle_x=panel_title_x(panel_rect), subtitle_width=panel_rect.w - 12)
    panel(a, panel_rect.x, panel_rect.y, panel_rect.w, panel_rect.h)
    content = fixed_content_rect(panel_rect, COMPARISON_PANEL_PAD_X, COMPARISON_PANEL_PAD_Y)
    label_w = min(label_w, content.w * 0.18)
    bar_w = content.w - label_w - label_gap
    x = content.x
    y = panel_rect.y + COMPARISON_PANEL_PAD_Y

    text(a, x, y + top_h / 2 + 4, top_label, 'label', 'start')
    segmented_bar(a, x + label_w + label_gap, y, bar_w, top_segments, h=top_h, gap=gap, min_widths=top_min_widths)

    bottom_y = y + top_h + row_gap
    text(a, x, bottom_y + bottom_h / 2 + 4, bottom_label, 'label', 'start')
    segmented_bar(a, x + label_w + label_gap, bottom_y, bar_w, bottom_segments, h=bottom_h, gap=gap, min_widths=bottom_min_widths)
    write_svg(path, finish(a))

def cyclic_buffer(path, title, subtitle, slots, queue_label, left_cursor_label, right_cursor_label, wrap_label, w=760, h=340):
    cell_gap = 14
    cell_w = max((landscape_box_dims(label, sub, min_w=86)[0] for label, cls, sub in slots), default=96)
    panel_content_w = len(slots) * cell_w + max(0, len(slots) - 1) * cell_gap
    cell_h = max(box_height(label, sub, cell_w) for label, cls, sub in slots)
    content_h = (
        CYCLIC_QUEUE_LABEL_H
        + CYCLIC_QUEUE_TO_CELLS
        + cell_h
        + CYCLIC_CELLS_TO_CURSOR
        + CYCLIC_CURSOR_LABEL_H
        + CYCLIC_CURSOR_TO_WRAP
        + CYCLIC_WRAP_BAND_H
    )
    panel_w = panel_content_w + CYCLIC_PANEL_PAD_X * 2
    h = svg_height_for_panel_content(content_h, CYCLIC_PANEL_PAD_Y, subtitle, panel_w - 12, panel_w)
    panel_w = panel_width_for_title(panel_w, title)
    svg_w = svg_width_for_panel(panel_w)
    panel_rect = main_panel_rect(h, SVG_OUTER_PAD_X, None, panel_w, subtitle, panel_w - 12)
    a = start(title, subtitle, svg_w, h, title_x=panel_title_x(panel_rect), subtitle_x=panel_title_x(panel_rect), subtitle_width=panel_rect.w - 12)
    panel(a, panel_rect.x, panel_rect.y, panel_rect.w, panel_rect.h)
    content = fixed_content_rect(panel_rect, CYCLIC_PANEL_PAD_X, CYCLIC_PANEL_PAD_Y)
    cells_rect = Rect(content.x, content.y + CYCLIC_QUEUE_LABEL_H + CYCLIC_QUEUE_TO_CELLS, content.w, cell_h)
    cell_frames = split_rect(cells_rect, [1] * len(slots), gap_ratio=0.025)
    for frame, (label, cls, sub) in zip(cell_frames, slots):
        box(a, frame.x, frame.y, frame.w, frame.h, label, cls, sub)

    text(a, cells_rect.cx, content.y + 4, queue_label, 'muted', 'middle')
    label_y = cells_rect.bottom + CYCLIC_CELLS_TO_CURSOR
    text(a, cell_frames[0].cx, label_y, left_cursor_label, 'muted', 'middle')
    text(a, cell_frames[-1].cx, label_y, right_cursor_label, 'muted', 'middle')
    wrap_y = label_y + CYCLIC_CURSOR_LABEL_H + CYCLIC_CURSOR_TO_WRAP
    wrap_bottom = wrap_y + CYCLIC_WRAP_BAND_H
    left_target_x = cell_frames[0].x + cell_frames[0].w * 0.18
    left_wrap_x = min(content.x, left_target_x - POLY_ARROW_MIN_END_TAIL)
    poly_arrow(a, [
        (cell_frames[-1].right - cell_frames[-1].w * 0.08, wrap_y),
        (content.right, wrap_y),
        (content.right, wrap_bottom),
        (left_wrap_x, wrap_bottom),
        (left_wrap_x, wrap_y),
        (left_target_x, wrap_y),
    ])
    edge_label(a, cells_rect.cx, wrap_bottom, wrap_label, 'middle')
    write_svg(path, finish(a))

def segmented_strip(path, title, subtitle, segments, start_label='', end_label='', w=760, h=300, gap_ratio=0.02, min_width_ratio=0.16):
    gap = 10
    natural_widths = [landscape_box_dims(label, sub, min_w=86)[0] for label, sub, *_ in segments]
    bar_w = sum(natural_widths) + max(0, len(segments) - 1) * gap
    label_h = SEGMENTED_LABEL_H + SEGMENTED_LABEL_GAP if (start_label or end_label) else 0
    bar_h = max(58, max(box_height(label, sub, max(86, width)) for (label, sub, *_), width in zip(segments, natural_widths)))
    content_h = label_h + bar_h
    panel_w = bar_w + SEGMENTED_PANEL_PAD_X * 2
    h = svg_height_for_panel_content(content_h, SEGMENTED_PANEL_PAD_Y, subtitle, panel_w - 12, panel_w)
    panel_w = panel_width_for_title(panel_w, title)
    svg_w = svg_width_for_panel(panel_w)
    panel_rect = main_panel_rect(h, SVG_OUTER_PAD_X, None, panel_w, subtitle, panel_w - 12)
    a = start(title, subtitle, svg_w, h, title_x=panel_title_x(panel_rect), subtitle_x=panel_title_x(panel_rect), subtitle_width=panel_rect.w - 12)
    panel(a, panel_rect.x, panel_rect.y, panel_rect.w, panel_rect.h)
    content = fixed_content_rect(panel_rect, SEGMENTED_PANEL_PAD_X, SEGMENTED_PANEL_PAD_Y)
    bar_y = content.y + label_h
    bar_rect = Rect(content.x, bar_y, content.w, bar_h)
    segmented_bar(
        a,
        bar_rect.x,
        bar_rect.y,
        bar_rect.w,
        segments,
        h=bar_rect.h,
        gap=gap,
        min_width=max(72, min(natural_widths)),
    )
    if start_label:
        text(a, bar_rect.x, content.y + 4, start_label, 'muted', 'start')
    if end_label:
        text(a, bar_rect.right, content.y + 4, end_label, 'muted', 'end')
    write_svg(path, finish(a))

# Chapter 1
flow(Path('ch01-diag01.svg'),'GRUB handoff state','Registers and boot metadata when control first reaches the kernel.',[
 ('EAX','multiboot magic','green'),('EBX','boot info pointer','blue'),('EIP','kernel entry point','amber'),('ESP','temporary stack','gray')])
stack(Path('ch01-diag02.svg'),'Entry stack after _start pushes Multiboot values','Stack grows downward; EAX lands at the lowest address and becomes the first argument.',[
 ('Initial ESP','Stack pointer undefined on entry','blue'),('EBX','Multiboot info pointer, 2nd argument','blue'),('EAX','Multiboot magic number, 1st argument','blue')],h=290)
stack(Path('ch01-diag03.svg'),'Kernel image layout','Sections grow upward from the load address, so the boot header sits at the lowest address.',[
 ('zeroed data','uninitialized globals, memory bitmap, kernel stack','gray'),('writable data','mutable global values','blue'),('read-only data','constant values','green'),('kernel code','executable instructions','amber'),('boot header','GRUB header, first 8 KB','blue')],h=310,top_note='higher addresses',bottom_note='lower addresses (kernel loaded at 0x100000)')
flow(Path('ch01-diag04.svg'),'AArch64 entry state','Register and privilege state when the primary core reaches the EL1 continuation after eret.',[
 ('Exception level','EL1 (dropped from EL2 via eret)','green'),('PC','kernel EL1 entry point (0x80000 + offset)','amber'),('SP_EL1','top of kernel stack region','blue'),('Secondary cores','parked in spin loop','gray')])
# Chapter 2
# Chapter 3
grid_snapshot(
    Path('ch03-diag01.svg'),
    'VGA text buffer',
    'Physical memory at 0xB8000 is a flat 80 x 25 text grid.',
    ('address', 'row', 'first cells', 'last cell'),
    [
        ('0xB8000', 'row 0', None),
        ('+160', 'row 1', '80 cells, 160 bytes'),
        ('+320', 'row 2', None),
        ('+3840', 'row 24', None),
    ],
    [
        (48, 'cell', "'H'"),
        (48, 'attr', '0x0F'),
        (48, 'cell', "'i'"),
        (48, 'attr', '0x0F'),
        (70, 'gap', None),
        (48, 'cell', "' '"),
        (48, 'attr', '0x0F'),
    ],
    notes=[
        'Each cell is two bytes: ASCII character, then attribute byte.',
        "Here the first two character cells spell 'Hi' on the screen.",
        '25 rows x 80 columns x 2 bytes = 4000 bytes.',
    ],
)
# Chapter 4
stack(Path('ch04-diag01.svg'),'Interrupt frame on the kernel stack','The trampoline saves one uniform frame before handing control to C.',[('Saved user stack','Only present on a ring-3 to ring-0 entry','gray'),('Interrupt return frame','Saved EFLAGS, CS, and EIP','green'),('Vector and error code','Raw interrupt identity and optional error detail','amber'),('General-purpose registers','Snapshot taken by pusha','blue'),('Segment registers','Saved DS, ES, FS, and GS','blue'),('Handler argument','Pointer to the saved frame passed to C','green')],h=372)
table(
    Path('ch04-diag02.svg'),
    'AArch64 exception vector table',
    'Sixteen 128-byte slots arranged as four exception classes across four contexts, addressed relative to VBAR_EL1.',
    ['Context', 'Synchronous', 'IRQ', 'FIQ', 'SError'],
    [
        ('Current EL, SP_EL0', '0x000', '0x080', '0x100', '0x180'),
        ('Current EL, SP_ELx', '0x200', '0x280', '0x300', '0x380'),
        ('Lower EL, AArch64',  '0x400', '0x480', '0x500', '0x580'),
        ('Lower EL, AArch32',  '0x600', '0x680', '0x700', '0x780'),
    ],
)
# Chapter 5
flow(Path('ch05-diag01.svg'),'Interrupt dispatch path','A hardware interrupt is turned into a driver callback inside the kernel.',[('Device interrupt','Timer or keyboard event','blue'),('Interrupt controller','CPU vector assignment','green'),('IRQ trampoline','Saved registers and interrupt frame','amber'),('Interrupt dispatcher','Handler lookup and EOI','blue'),('Driver callback','Device-specific handler code','green')],h=420)
compare_timelines(Path('ch05-diag02.svg'),'RTC seed and PIT upkeep','Boot reads the hardware clock once; runtime keeps time by counting timer ticks.', 'Boot', [('Read RTC', 'stable calendar sample', 'blue'), ('Decode fields', 'convert from BCD if needed', 'green'), ('Seed Unix time', 'initial seconds counter', 'amber')], 'Runtime', [('Timer tick', '100 Hz IRQ 0', 'blue'), ('Count to 100', 'subsecond accumulator', 'green'), ('Add one second', 'advance Unix time', 'amber')], preferred_w=150, gap=18, row_gap=42, h=360)
# Chapter 7
# Chapter 8
stack(Path('ch08-diag01.svg'),'Physical memory map','Low memory is carved up before the page allocator manages extended RAM.',[('extended RAM','managed by the page allocator','green'),('kernel image + BSS','kernel code, data, page bitmap, and boot stack','blue'),('video + firmware area','reserved','gray'),('kernel heap','0x32000-0x8FFFF','green'),('early page maps','identity-mapped paging structures','blue'),('firmware tables','reserved low memory','gray')],h=390)
flow(Path('ch08-diag02.svg'),'Page lookup','The CPU follows paging metadata to turn a virtual address into a physical byte.',[('root page map','top-level paging structure','blue'),('directory entry','points at a page table','green'),('table entry','points at a physical page','amber'),('page + offset','final byte address','blue')],h=260)
segmented_strip(
    Path('ch08-diag03.svg'),
    'Initial kernel heap',
    'Immediately after initialization, the heap is one block header followed by one large free payload region.',
    [
        ('Header', '16-byte metadata', 'blue', 1),
        ('Free payload', 'entire remaining heap', 'green', 8),
    ],
    start_label='Heap start',
    end_label='Heap end',
)
compare_segments(Path('ch08-diag04.svg'),'Heap split on allocation','Allocation is a before/after change: one large free block becomes one allocated block plus one smaller free block.', 'Before', [('Free block', '4000-byte payload', 'green', 9)], 'After', [('Allocated block', '64-byte payload', 'blue', 1), ('Remaining free', '3920-byte payload', 'green', 8)], h=332)
compare_segments(Path('ch08-diag05.svg'),'Heap coalescing','Coalescing is another before/after change: adjacent free neighbours become one larger reusable block.', 'Before', [('Free block A', '64 B', 'green', 2), ('Free block B', '32 B', 'green', 1), ('Free block C', '128 B', 'green', 4)], 'After', [('Merged block', '256 B plus absorbed headers', 'amber', 7)], h=332)
stack(Path('ch08-diag06.svg'),'Slab page layout','Each slab page starts with metadata, then packs fixed-size object slots.',[('page header','next pointer, free count, free-list head','blue'),('object slot 0','','green'),('object slot 1','','green'),('more object slots','','green'),('unused padding','space smaller than one full object','gray')],h=320)
flow(Path('ch08-diag07.svg'),'Slab free list','Each free slot points at the next one, starting from the free-list head.',[('free-list head','','amber'),('free slot 0','','green'),('free slot 1','','green'),('last free slot','','green'),('end of list','','gray')],h=245)
# Chapter 9
tagged_transcript(Path('ch09-diag01.svg'), 'Kernel log format', 'Each log line starts with a subsystem tag, then the human-readable message.',
                  [('MEM', 'free pages: 31500'), ('DISK', 'drive ready'), ('FILES', 'filesystem mounted'), ('FAULT', 'page-fault exception')], h=258)
tagged_transcript(Path('ch09-diag02.svg'), 'Typical boot log', 'Startup reads as a structured transcript of subsystem initialization, with one tagged line per subsystem event.',
                  [('MEM', 'boot memory map found'), ('MEM', 'free pages: 31500'), ('CPU', 'user-mode entry state installed'), ('DISK', 'disk initialized'), ('TRAPS', 'interrupts enabled'),
                   ('FILES', 'filesystem mounted'), ('FILES', 'shell program located'), ('PROCESS', 'entering user mode')], h=310)
# Chapter 10
cyclic_buffer(
    Path('ch10-diag01.svg'),
    'Keyboard ring buffer',
    'Head and tail move around a fixed array; when either index reaches the end, it wraps to slot 0.',
    [
        ('tail', 'amber', ''),
        ('1', 'blue', ''),
        ('2', 'blue', ''),
        ('3', 'blue', ''),
        ('4', 'blue', ''),
        ('5', 'blue', ''),
        ('6', 'blue', ''),
        ('head', 'green', ''),
    ],
    queue_label='Queued bytes in the fixed 256-byte array',
    left_cursor_label='next read',
    right_cursor_label='next write',
    wrap_label='wrap to slot 0',
)
# Chapter 11
# Chapter 12
# Chapter 13
stack(Path('ch13-diag01.svg'),'DUFS disk layout','DUFS v3 reserves metadata sectors before file data blocks.',[('sector 0','unused sentinel','gray'),('sector 1','superblock','blue'),('sectors 2-9','inode bitmap','green'),('sectors 10-17','block bitmap','amber'),('sectors 18-273','inode table','blue'),('sector 274+','data region','green')],h=360)
flow(Path('ch13-diag02.svg'),'Inode block pointers','Small files use direct block pointers; larger files use lookup tables.',[('direct blocks','first 12 file blocks','blue'),('indirect table','table of more block pointers','green'),('double-indirect','table that points at more tables','amber'),('data blocks','4 KB blocks of file data','blue')],h=280)
buffer_transfer(
    Path('ch13-diag03.svg'),
    'Read one filesystem block',
    'One 4 KB filesystem block is assembled from eight consecutive disk sectors.',
    'disk sectors within the block',
    '4 KB buffer',
    [f'+{i}' for i in range(8)],
    'buffer',
    '4096 B',
)
flow(Path('ch13-diag04.svg'),'Path resolution','Opening a path means walking directory entries until the final file is found.',[('root directory','named project folder','blue'),('project folder','named source folder','green'),('source folder','named target file','amber'),('target file','opened by the kernel','blue')],h=270)
lane_walk(
    Path('ch13-diag05.svg'),
    'Logical block lookup',
    'The inode chooses a direct, indirect, or double-indirect pointer path from the logical block index.',
    [
        ('blocks 0-11', [('inode direct pointer', 'blue')]),
        ('blocks 12-1035', [('sector table', 'blue'), ('data', 'green')]),
        ('blocks 1036+', [('L0', 'blue'), ('L1', 'amber'), ('data', 'green')]),
    ],
)
segmented_strip(
    Path('ch13-diag06.svg'),
    'One inode-table sector',
    'Each 512-byte sector is divided into four consecutive 128-byte inode slots.',
    [
        ('Offset 0', 'first inode', 'blue', 1),
        ('Offset 128', 'second inode', 'blue', 1),
        ('Offset 256', 'third inode', 'blue', 1),
        ('Offset 384', 'fourth inode', 'blue', 1),
    ],
    start_label='Sector start',
    end_label='Sector end (512 B)',
)
split_overview(
    Path('ch13-diag07.svg'),
    'New filesystem image',
    'A 50 MiB disk image with metadata regions at the front and starter files in the data area.',
    'disk layout',
    [
        ('1', 'superblock', 'blue', 66),
        ('2-9', 'inode bitmap', 'green', 112),
        ('10-17', 'block bitmap', 'amber', 112),
        ('18-273', 'inode table', 'blue', 170),
        ('274+', 'data region', 'green', 158),
    ],
    'starter files',
    [
        ('/', 'root directory entry'),
        ('/shell', 'shell program in the data region'),
        ('/date', 'date program stored later in the data region'),
    ],
)
flow(Path('ch13-diag08.svg'),'Ext3 journaled mutation','The journal records a committed block image before the home location is treated as durable.',[
    ('stage blocks','full 4 KB images in memory','blue'),
    ('mark recovery','set ext3 needs_recovery','amber'),
    ('write journal','descriptor, data, commit','green'),
    ('checkpoint','copy images to home blocks','blue'),
    ('clean journal','clear start and advance sequence','green')
],h=420)
# Chapter 14
stack(Path('ch14-diag01.svg'),'File operation stack','A file call descends through a fixed layer stack from user space to the disk driver.',[('User program','read, write, or open request','blue'),('Kernel entry layer','fd checks and syscall entry','green'),('VFS router','choose the owning backend','amber'),('Filesystem implementation','inode and directory logic','blue'),('Disk I/O layer','sector-sized requests','green'),('Disk driver','talk to hardware','gray')],h=450)
# Chapter 15
stack(Path('ch15-diag01.svg'),'User-mode entry stack','The kernel prepares this stack frame so the CPU can jump into user mode.',[('user data segment','which data segment user mode should use','green'),('user stack pointer','top of the first user stack','green'),('processor flags','initial interrupt and flag state','blue'),('user code segment','which code segment user mode should use','green'),('first user instruction','where execution begins','amber')],h=330)
state_machine(Path('ch15-diag02.svg'),'Process state machine','Processes move between ready, running, blocked, zombie, and unused states as the scheduler and syscalls act on them.',[
 ('unused',0.00,0.16,0.16,0.16,'unused slot','free process-table entry','gray'),
 ('ready',0.28,0.16,0.20,0.16,'ready to run','eligible for scheduling','green'),
 ('running',0.60,0.16,0.12,0.16,'running','currently on the CPU','blue'),
 ('finished',0.82,0.16,0.18,0.16,'zombie','exited, waiting to be reaped','gray'),
 ('waiting',0.56,0.71,0.20,0.18,'waiting','blocked on an event','amber')
],[
 ('unused','e','ready','w',[],'create',(0.22,0.16),'middle'),
 ('ready','e','running','w',[],'schedule',(0.54,0.16),'middle'),
 ('running','n','ready','n',[(0.66,0.05),(0.38,0.05)],'preempt / yield',(0.52,0.01),'middle'),
 ('running','s','waiting','n',[],'block',(0.71,0.56),'start'),
 ('waiting','w','ready','s',[(0.38,0.80)],'wake',(0.44,0.64),'middle'),
 ('running','e','finished','w',[],'exit',(0.77,0.16),'middle'),
 ('finished','s','unused','s',[(0.91,0.93),(0.08,0.93)],'reap',(0.48,0.985),'middle')
], w=820, h=520, panel_w=752, relative=True)
stack(Path('ch15-diag03.svg'),'Initial user stack','The kernel builds argument strings, pointer arrays, and the initial stack values.',[('environment text','environment variable strings at high addresses','green'),('argument text','program argument strings stored below them','green'),('environment pointers','envp array pointing at environment strings','blue'),('argument pointers','argv array pointing at argument strings','blue'),('entry values','argument count and initial stack pointer','amber')],h=330)
flow(Path('ch15-diag04.svg'),'Forked kernel stacks','The child gets a copied trap frame with the return value changed to zero.',[('parent frame','parent sees child id','blue'),('copied frame','same saved state','green'),('child frame','child sees zero','amber'),('two returns','parent and child continue','blue')],h=285)
# Chapter 16
compare_timelines(Path('ch16-diag01.svg'),'System call handoff','User mode loads registers and traps; kernel mode saves a frame, dispatches the handler, then returns through `iret`.', 'User mode', [('Load registers', '`EAX` number, other args', 'blue'), ('`int 0x80`', 'controlled ring transition', 'amber'), ('Resume', 'return value restored in `EAX`', 'green')], 'Kernel mode', [('Trap gate entry', 'switch to the kernel stack', 'blue'), ('Save syscall frame', 'preserve user registers', 'green'), ('Dispatch handler', 'choose by syscall number', 'amber'), ('`iret`', 'restore saved user state', 'green')], preferred_w=138, gap=16, row_gap=40, h=372)
# Chapter 17
# Chapter 18
flow(Path('ch18-diag01.svg'),'Keyboard to terminal path','A keypress is decoded, queued in the terminal buffer, and eventually copied to user space.',[('keyboard interrupt','raw scancode from hardware','blue'),('key decoder','turn scancode into a character','green'),('terminal input buffer','raw or line-buffered queue','amber'),('wake blocked reader','reader can run again','blue'),('user read returns','copy bytes to user space','green')],h=380)
tree(Path('ch18-diag02.svg'),'Terminal read behavior','A terminal read branches on the active input mode and on whether enough buffered input is already available.',[
 ('read',0.35,0.02,0.30,0.11,'read request','fd and user buffer','blue'),
 ('canon',0.35,0.24,0.30,0.11,'canonical mode?','line discipline check','amber'),
 ('lineq',0.00,0.48,0.27,0.11,'full line ready?','canonical buffer check','amber'),
 ('rawq',0.73,0.48,0.27,0.11,'raw bytes ready?','byte-availability check','amber'),
 ('sleep',0.35,0.72,0.30,0.11,'sleep for input','block until input wakes the reader','gray'),
 ('copyline',0.00,0.88,0.27,0.11,'copy line','return completed line','green'),
 ('copyraw',0.73,0.88,0.27,0.11,'copy bytes','return available bytes','green')
],[
 ('read','canon',''),
 ('canon','lineq','yes','w','n'),
 ('canon','rawq','no','e','n'),
 ('lineq','copyline','yes','s','n'),
 ('lineq','sleep','no','e','n'),
 ('rawq','copyraw','yes','s','n'),
 ('rawq','sleep','no','w','n')
], w=820, h=680, panel_x=34, panel_w=752, relative=True)
# Chapter 19
flow(Path('ch19-diag01.svg'),'Signal lifecycle','Signals are marked pending, then delivered at the user-mode return boundary.',[('send signal','mark it pending','blue'),('wake target','if blocked','green'),('return to user mode','safe delivery point','amber'),('build frame','prepare handler call','blue'),('resume helper','restore saved state','green')],h=380)
stack(Path('ch19-diag02.svg'),'Signal frame','The kernel saves user context, then builds a small frame to enter the signal handler.',[('return helper','where the handler returns','amber'),('signal number','argument passed to the handler','blue'),('saved user context','state restored after the handler finishes','green'),('resume helper','tiny code that asks the kernel to continue','gray')],h=310)
# Chapter 20
timeline(Path('ch20-diag01.svg'),'User runtime entry','The kernel prepares the initial stack, `_start` turns that frame into a C call to `main`, and the return value flows to `exit`.',[('initial user stack','kernel writes argc, argv, and envp','blue'),('`_start`','store `environ`, then call `main`','green'),('`main(argc, argv, envp)`','run user code','amber'),('`sys_exit(ret)`',"return `main`'s value to the kernel",'blue')], preferred_w=140, gap=16, h=332)
# Chapter 21
stack(Path('ch21-diag01.svg'),'C library layers','Higher library pieces are built on top of simpler helper layers.',[('stream I/O layer','formatted input and output helpers','blue'),('utility layer','general helpers built on strings and kernel calls','green'),('character helpers','tests and transforms single characters','gray'),('string helpers','copy, compare, and scan strings','gray'),('kernel-call wrapper','thin layer over the kernel ABI','amber')],h=300)
# Chapter 22
# Chapter 23
flow(Path('ch23-diag01.svg'),'Module loading path','The loader reads the relocatable object, builds one runtime image, resolves symbols, applies relocations, and finally runs `module_init`.',[('read ELF object','module bytes come from DUFS','blue'),('size runtime image','sum `SHF_ALLOC` sections','green'),('lay out sections','copy text, data, and zeroed BSS','amber'),('resolve + relocate','match exports and patch references','blue'),('call `module_init`','module becomes live on success','green')],h=430)
# Chapter 24
stack(Path('ch24-diag01.svg'),'Core dump file layout','An ELF core file begins with metadata, then stores Linux-compatible notes, Drunix forensics notes, and page-aligned memory segments.',[('ELF header','file type is `ET_CORE`','blue'),('Program header table','one `PT_NOTE` header plus `PT_LOAD` headers','green'),('CORE notes','`NT_PRSTATUS` and `NT_PRPSINFO` records','amber'),('DRUNIX notes','`vmstat`, `fault`, and `maps` text records','green'),('Load segments','copied user pages from the crashed process','blue')],h=372)
# Chapter 25
tree(Path('ch25-diag01.svg'),'Page-fault decision tree','Recoverable user faults allocate heap or stack pages, or resolve copy-on-write, before the instruction is retried.',[
 ('fault',0.37,0.02,0.26,0.13,'page fault','CR2 + error code saved','blue'),
 ('user',0.37,0.30,0.26,0.13,'U bit set?','ring 3 fault','amber'),
 ('kernel',0.79,0.30,0.18,0.13,'kernel fault','print state and halt','gray'),
 ('missing',0.37,0.58,0.26,0.13,'P bit clear?','missing translation','amber'),
 ('demand',0.00,0.58,0.26,0.13,'heap or stack page','allocate and retry','green'),
 ('cow',0.37,0.86,0.26,0.13,'P=1, W=1, PG_COW?','present write case','amber'),
 ('resolve',0.00,0.86,0.26,0.13,'resolve fault','promote or copy, then retry','green'),
 ('sigsegv',0.79,0.86,0.18,0.13,'SIGSEGV','unrecoverable user fault','gray')
],[
 ('fault','user','',),
 ('user','kernel','U=0'),
 ('user','missing','U=1'),
 ('missing','demand','P=0'),
 ('missing','cow','P=1'),
 ('cow','resolve','yes'),
 ('cow','sigsegv','no')
],w=860,h=664,panel_x=34,panel_w=792, relative=True)
timeline(Path('ch25-diag02.svg'),'Demand paging installs the missing PTE','`brk()` reserves the virtual range first; the first touch is what allocates and maps the physical page.',[
 ('before first touch','break moved forward, but the PTE is not present','gray'),
 ('fault path','CPU records CR2, kernel allocates and zeroes one page','amber'),
 ('after retry','PTE is present and user-writable, so the instruction succeeds','green')
],h=304)
# Chapter 26
flow(Path('ch26-diag01.svg'),'Frame refcount lifecycle','The same physical page moves from one owner to two shared owners, then splits back into private pages.',[
 ('before fork','one frame, refcount 1','blue'),
 ('after fork','parent and child share it, refcount 2','green'),
 ('after write fault','one side gets a private copy','amber'),
 ('after exit','last owner drops the final reference','gray')
],h=300)
state_machine(Path('ch26-diag02.svg'),'PTE state machine','Writable pages become copy-on-write during `fork()`, while true read-only pages stay read-only.',[
 ('fork',0.34,0.04,0.32,0.18,'fork inspects user PTE','decide whether the page is writable','blue'),
 ('writable',0.05,0.42,0.30,0.18,'writable page','PG_WRITABLE set, PG_COW clear','green'),
 ('readonly',0.65,0.42,0.30,0.18,'read-only page','PG_WRITABLE clear, PG_COW clear','gray'),
 ('cow',0.05,0.78,0.30,0.16,'shared CoW PTE','later write promotes or copies','amber'),
 ('roshare',0.65,0.78,0.30,0.16,'shared read-only PTE','later write still faults','gray')
],[
 ('fork','w','writable','n',[],'writable',(0.07,0.27),'start'),
 ('fork','e','readonly','n',[],'read-only',(0.83,0.27),'start'),
 ('writable','s','cow','n',[],'after fork',(0.07,0.70),'start'),
 ('readonly','s','roshare','n',[],'after fork',(0.83,0.70),'start')
],w=860,h=560,panel_w=792, relative=True)
# Chapter 28
compare_segments(Path('ch28-diag01.svg'),'Two display modes share one compositor','Above the display layer the desktop is mode-oblivious; only the bottom-most present step knows whether it is writing to VGA text memory or rasterising into a pixel back buffer.',
                 'VGA text',
                 [('Cell grid', 'character + attribute', 'blue', 4),
                  ('Direct write to 0xB8000', 'one cell per 16 bits', 'green', 5)],
                 'Framebuffer',
                 [('Cell grid', 'character + attribute', 'blue', 4),
                  ('Glyph rasterisation', '8x16 bitmap per cell', 'amber', 4),
                  ('Back buffer + present', 'dirty rect to visible framebuffer', 'green', 5)],
                 h=360)
flow(Path('ch28-diag02.svg'),'Cursor overlay during present','The cursor is never written to either buffer. Each present copies back-buffer pixels to the front, then overlays the cursor sprite where the cursor rect intersects the presented rectangle.',[
 ('Compositor draws windows','into back buffer only','blue'),
 ('framebuffer_present_rect','copy dirty rect to front','green'),
 ('Overlay cursor sprite','for pixels inside cursor rect','amber'),
 ('Visible framebuffer','what the user sees','blue')
],h=380)
stack(Path('ch28-diag03.svg'),'Window z-order at present time','The compositor walks the stack from bottom to top; whichever window has the highest z value paints last and ends up on top.',[
 ('Launcher overlay (when open)','absorbs input above all else','amber'),
 ('Taskbar strip','top-of-screen chrome','blue'),
 ('Focused window','highest z, painted last','green'),
 ('Other windows','painted in z order','blue'),
 ('Desktop background','painted first','gray')
],h=420)
# Chapter 29
flow(Path('ch29-diag01.svg'),'How `make debug` sets up GDB','QEMU opens the remote stub, GDB loads local symbols, then the two exchange state until you tell the CPU to run.',[
 ('QEMU `-s -S`','open port, keep CPU paused','gray'),
 ('GDB `file kernel.elf`','load symbols locally','blue'),
 ('`target remote`','connect to QEMU stub','blue'),
 ('Inspect State','read regs and memory','blue'),
 ('Type `continue`','CPU starts running','amber')
],h=430)
tree(Path('ch29-diag02.svg'),'Where GDB and interrupts meet','A breakpoint can stop early, but `next` succeeds only if the guest already has a valid trap path.',[
 ('cont',0.35,0.01,0.30,0.07,'GDB says continue','resume the CPU','blue'),
 ('bp',0.35,0.16,0.30,0.07,'Hardware breakpoint','CPU stops here','amber'),
 ('inspect',0.35,0.31,0.30,0.07,'Inspect state','source and registers are visible','blue'),
 ('next',0.35,0.46,0.30,0.07,'GDB says `next`','resume for the next line','blue'),
 ('trap',0.35,0.61,0.30,0.07,'CPU must trap again','this stop uses the guest trap path','amber'),
 ('idt',0.03,0.76,0.28,0.07,'IDT loaded','trap enters a valid path','green'),
 ('noidt',0.69,0.76,0.28,0.07,'No IDT','no valid trap path','gray'),
 ('clean',0.03,0.92,0.28,0.07,'Clean stop','control returns to GDB','green'),
 ('bad',0.69,0.92,0.28,0.07,'Bad stop','execution goes astray','gray')
],[
 ('cont','bp',''),
 ('bp','inspect',''),
 ('inspect','next',''),
 ('next','trap',''),
 ('trap','idt','IDT yes','w','n'),
 ('trap','noidt','IDT no','e','n'),
 ('idt','clean',''),
 ('noidt','bad','')
],w=860,h=1048,panel_x=34,panel_w=792, relative=True)
# Chapter 30
timeline(Path('ch30-diag01.svg'),'C++ user program startup','The kernel still enters `_start`; the user runtime runs language hooks around `main` before exiting.',[
 ('initial user stack','kernel writes argc, argv, and envp','blue'),
 ('constructors','walk `.init_array` and `.ctors`','green'),
 ('`main(argc, argv, envp)`','run the C++ program','amber'),
 ('destructors','walk `.fini_array` and `.dtors`','green'),
 ('`sys_exit(ret)`','return `main` status to the kernel','blue')
], preferred_w=140, gap=16, h=332)
stack(Path('ch30-diag02.svg'),'Userland C++ runtime layers','C++ support is a thin language layer on top of the existing C user runtime.',[
 ('C++ user program','classes, globals, virtual calls, and allocation','blue'),
 ('C++ runtime layer','constructors, destructors, operators, and ABI hooks','green'),
 ('C user runtime and libc','startup, syscalls, heap, strings, and stdio','amber'),
 ('Kernel ABI','`int 0x80`, file descriptors, and `SYS_BRK`','blue')
],h=340)

validate_generated_diagrams()
