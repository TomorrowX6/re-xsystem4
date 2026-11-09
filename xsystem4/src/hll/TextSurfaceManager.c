/* Sys43 glyph metrics for text layout, GPL-2.0-or-later. */
#include <math.h>
#include "system4.h"
#include "system4/string.h"
#include "gfx/font.h"
#include "hll.h"

static bool GetFontWidth(struct string *text, int *width, int face, int size,
		possibly_unused int r, possibly_unused int g, possibly_unused int b,
		float bold, float edge, possibly_unused int edge_r,
		possibly_unused int edge_g, possibly_unused int edge_b)
{
	*width = 0;
	if (!text || !text->size || size <= 0 || !isfinite(bold) || !isfinite(edge))
		return false;
	gfx_font_init();
	struct text_style style = { .face = face, .size = size, .scale_x = 1.f, .space_scale_x = 1.f };
	// This API measures the first character, even when given a longer string.
	// Each side reserves the larger of the bold and outline extents.
	*width = ceilf(gfx_size_char_unstyled(&style, text->text))
		+ 2 * ceilf(max(0.f, max(bold, edge)));
	return true;
}

HLL_LIBRARY(TextSurfaceManager, HLL_EXPORT(GetFontWidth, GetFontWidth));
