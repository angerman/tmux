/* $OpenBSD$ */

/*
 * Copyright (c) 2026 Nicholas Marriott <nicholas.marriott@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

/*
 * Fuzzy subsequence matching in the style of fzf. The pattern is split on
 * spaces into tokens and every token must match as a subsequence (its
 * characters appearing in order, not necessarily consecutively) somewhere in
 * the text, so "foo bar" requires both "foo" and "bar" to be present. Each
 * token is matched greedily and independently from the start of the text.
 *
 * Both the pattern and the text are UTF-8. The text may contain tmux style
 * directives (#[...]); these and their contents are invisible to matching and
 * occupy no columns, but align= styles do move the surrounding text and are
 * accounted for exactly as format_draw lays it out (the no-list layout, see
 * format_draw_none). Matching is smart-case: case is ignored unless the pattern
 * contains an uppercase character (ASCII case folding only; other characters
 * are compared by codepoint).
 *
 * On a match a bitstr_t of the requested display width is returned with a bit
 * set for every column occupied by a matched character, so the caller can
 * highlight them; NULL is returned if there is no match. A cheap fzf-style
 * score (matches at the start, after word boundaries and in contiguous runs
 * score higher) is also produced so callers can rank best-match-first.
 */

#define FUZZY_BONUS_START 12
#define FUZZY_BONUS_BOUNDARY 8
#define FUZZY_BONUS_CONSECUTIVE 6
#define FUZZY_PENALTY_LEADING 1
#define FUZZY_PENALTY_LEADING_MAX 10

/* A single visible character of the text. */
struct fuzzy_char {
	enum style_align	 align;
	struct utf8_data	 ud;		/* original UTF-8 data */
	u_int			 width;		/* display width */
	u_int			 offset;	/* within its alignment */
};

/* Is this character a word boundary, so a match after it scores higher? */
static int
fuzzy_is_boundary(const struct utf8_data *ud)
{
	u_char	c;

	if (ud->size != 1)
		return (0);
	c = ud->data[0];
	return (c == ' ' || c == '-' || c == '_' || c == '/' ||
	    c == '.' || c == ':');
}

/* Compare two characters, folding ASCII case if wanted. */
static int
fuzzy_char_equal(const struct utf8_data *a, const struct utf8_data *b, int fold)
{
	/*
	 * Single-byte ASCII on both sides can be compared directly, applying
	 * smart-case folding if wanted. Anything else (multibyte UTF-8 or a raw
	 * literal byte) is compared exactly by size and data, with no case
	 * folding.
	 */
	if (fold && a->size == 1 && b->size == 1 &&
	    a->data[0] < 0x80 && b->data[0] < 0x80)
		return (tolower(a->data[0]) == tolower(b->data[0]));
	return (a->size == b->size && memcmp(a->data, b->data, a->size) == 0);
}

/* Map a style alignment onto one of the four layout columns. */
static enum style_align
fuzzy_align(enum style_align align)
{
	if (align == STYLE_ALIGN_DEFAULT)
		return (STYLE_ALIGN_LEFT);
	return (align);
}

/* Add a visible character to the array, updating the alignment width. */
static void
fuzzy_add(struct fuzzy_char **cs, u_int *ncs, u_int *alloc, enum style_align a,
    const struct utf8_data *ud, u_int *widths)
{
	struct fuzzy_char	*fc;

	if (*ncs == *alloc) {
		*alloc = (*alloc == 0) ? 64 : *alloc * 2;
		*cs = xreallocarray(*cs, *alloc, sizeof **cs);
	}
	fc = &(*cs)[(*ncs)++];
	fc->align = a;
	memcpy(&fc->ud, ud, sizeof fc->ud);
	fc->width = ud->width;
	fc->offset = widths[a];
	widths[a] += ud->width;
}

/*
 * Decode the character at cp (which must be before end) into ud and return a
 * pointer to the byte after it. A valid multibyte sequence is consumed whole
 * and ud holds its UTF-8 data and width. On a decode failure, or for a plain
 * single byte, the original start byte is stored as a literal (width 1) and
 * the pointer advances by exactly one. This is the single decode path shared
 * by the text and pattern sides so they can never diverge.
 */
static const char *
fuzzy_decode_one(const char *cp, const char *end, struct utf8_data *ud)
{
	enum utf8_state	 more;
	const char	*start = cp;

	if ((more = utf8_open(ud, (u_char)*cp)) == UTF8_MORE) {
		while (++cp != end && more == UTF8_MORE)
			more = utf8_append(ud, (u_char)*cp);
		if (more == UTF8_DONE)
			return (cp);	/* cp is one past the sequence */
		cp = start;		/* decode failed, fall back to literal */
	}
	utf8_set(ud, (u_char)*cp);
	return (cp + 1);
}

/*
 * Scan the text into an array of visible characters, skipping styles and
 * recording the alignment and intra-alignment offset of each. Returns the
 * array and its length and fills in the per-alignment widths.
 */
static struct fuzzy_char *
fuzzy_scan(const char *text, u_int *ncs, u_int *widths)
{
	struct fuzzy_char	*cs = NULL;
	u_int			 alloc = 0, n, leading, i;
	enum style_align	 current = STYLE_ALIGN_LEFT;
	struct style		 sy;
	const char		*cp = text, *textend = text + strlen(text), *end;
	struct utf8_data	 ud, hash, bracket;
	char			*tmp;

	*ncs = 0;
	memset(widths, 0, sizeof *widths * (STYLE_ALIGN_ABSOLUTE_CENTRE + 1));
	style_set(&sy, &grid_default_cell);
	utf8_set(&hash, '#');
	utf8_set(&bracket, '[');

	while (*cp != '\0') {
		/* Handle a run of #s, which may introduce a style. */
		if (*cp == '#') {
			for (n = 0; cp[n] == '#'; n++)
				/* nothing */;
			if (cp[n] != '[') {
				/* Escaped #s: ##->#, so half (rounded up). */
				leading = (n % 2 == 0) ? n / 2 : n / 2 + 1;
				for (i = 0; i < leading; i++) {
					fuzzy_add(&cs, ncs, &alloc, current,
					    &hash, widths);
				}
				cp += n;
				continue;
			}
			/* Even count: all #s escaped, the [ is literal. */
			for (i = 0; i < n / 2; i++)
				fuzzy_add(&cs, ncs, &alloc, current, &hash,
				    widths);
			if (n % 2 == 0) {
				fuzzy_add(&cs, ncs, &alloc, current, &bracket,
				    widths);
				cp += n + 1;
				continue;
			}

			/* Odd count: this is a style, find and parse it. */
			end = format_skip(cp + n + 1, "]");
			if (end == NULL)
				break;
			tmp = xstrndup(cp + n + 1, end - (cp + n + 1));
			if (style_parse(&sy, &grid_default_cell, tmp) == 0)
				current = fuzzy_align(sy.align);
			free(tmp);
			cp = end + 1;
			continue;
		}

		/* Decode one character, multibyte or single byte. */
		cp = fuzzy_decode_one(cp, textend, &ud);

		/*
		 * Skip non-printable single bytes (control characters and raw
		 * bytes left over from a failed decode); keep printable ASCII
		 * and any decoded UTF-8.
		 */
		if (ud.size == 1 && (ud.data[0] <= 0x1f || ud.data[0] >= 0x7f))
			continue;
		fuzzy_add(&cs, ncs, &alloc, current, &ud, widths);
	}
	return (cs);
}

/*
 * Work out the display column of a visible character given the trimmed widths
 * and start columns of each alignment. Returns 0 and sets the column if the
 * character is visible, otherwise returns -1.
 */
static int
fuzzy_column(const struct fuzzy_char *fc, const u_int *start, const u_int *src,
    const u_int *vis, u_int *column)
{
	enum style_align	a = fc->align;

	if (fc->offset < src[a] || fc->offset >= src[a] + vis[a])
		return (-1);
	*column = start[a] + (fc->offset - src[a]);
	return (0);
}

/*
 * Match a single token (no spaces) as a subsequence of the visible characters,
 * accumulating the score and flagging matched characters. Returns 1 if the
 * token matches and 0 if not.
 */
static int
fuzzy_match_token(const struct utf8_data *token, u_int tokenlen,
    struct fuzzy_char *cs, u_int ncs, int fold, int *score, char *matched)
{
	u_int	pi = 0, ci = 0, last = 0;
	int	started = 0;

	while (pi != tokenlen) {
		while (ci != ncs &&
		    !fuzzy_char_equal(&token[pi], &cs[ci].ud, fold))
			ci++;
		if (ci == ncs)
			return (0);

		if (!started) {
			if (ci == 0)
				*score += FUZZY_BONUS_START;
			else {
				if (fuzzy_is_boundary(&cs[ci - 1].ud))
					*score += FUZZY_BONUS_BOUNDARY;
				if (ci < FUZZY_PENALTY_LEADING_MAX)
					*score -= ci * FUZZY_PENALTY_LEADING;
				else {
					*score -= FUZZY_PENALTY_LEADING_MAX *
					    FUZZY_PENALTY_LEADING;
				}
			}
			started = 1;
		} else {
			if (ci == last + 1)
				*score += FUZZY_BONUS_CONSECUTIVE;
			else if (fuzzy_is_boundary(&cs[ci - 1].ud))
				*score += FUZZY_BONUS_BOUNDARY;
		}
		last = ci;

		matched[ci] = 1;
		pi++;
		ci++;
	}
	return (1);
}

/*
 * Decode a UTF-8 pattern token into an array of characters, using the same
 * decode path as the text side. Returns the count.
 */
static u_int
fuzzy_decode(const char *token, size_t len, struct utf8_data *out)
{
	const char	*cp = token, *end = token + len;
	u_int		 n = 0;

	while (cp != end)
		cp = fuzzy_decode_one(cp, end, &out[n++]);
	return (n);
}

/*
 * Fuzzy match pattern against text, which is drawn into a region of the given
 * display width. Returns a bitstr_t of width bits with a bit set for each
 * column occupied by a matched character, or NULL if there is no match. If
 * score is not NULL it is set to the match score (higher is better) on a match.
 */
bitstr_t *
fuzzy_match(const char *pattern, const char *text, u_int width, u_int *score)
{
	struct fuzzy_char	*cs;
	char			*matched = NULL;
	struct utf8_data	*token;
	bitstr_t		*mask;
	u_int			 ncs, i, j, column;
	u_int			 widths[STYLE_ALIGN_ABSOLUTE_CENTRE + 1];
	u_int			 start[STYLE_ALIGN_ABSOLUTE_CENTRE + 1];
	u_int			 src[STYLE_ALIGN_ABSOLUTE_CENTRE + 1];
	u_int			 vis[STYLE_ALIGN_ABSOLUTE_CENTRE + 1];
	u_int			 wl, wc, wr, wa;
	const char		*cp, *sp;
	int			 total = 0, fold;

	if (width == 0)
		return (NULL);

	/* Smart-case: fold unless the pattern has an uppercase character. */
	fold = 1;
	for (cp = pattern; *cp != '\0'; cp++) {
		if (*cp >= 'A' && *cp <= 'Z') {
			fold = 0;
			break;
		}
	}

	/* Scan the text into visible characters. */
	cs = fuzzy_scan(text, &ncs, widths);
	matched = xcalloc(ncs == 0 ? 1 : ncs, sizeof *matched);
	token = xreallocarray(NULL, strlen(pattern) + 1, sizeof *token);

	/* Match each space-separated token as a subsequence. */
	cp = pattern;
	while (*cp != '\0') {
		while (*cp == ' ')
			cp++;
		if (*cp == '\0')
			break;
		sp = cp;
		while (*cp != '\0' && *cp != ' ')
			cp++;
		i = fuzzy_decode(sp, cp - sp, token);
		if (!fuzzy_match_token(token, i, cs, ncs, fold, &total,
		    matched)) {
			free(token);
			free(matched);
			free(cs);
			return (NULL);
		}
	}
	free(token);

	/*
	 * Work out the trimmed widths and start columns of each alignment,
	 * mirroring format_draw_none.
	 */
	wl = widths[STYLE_ALIGN_LEFT];
	wc = widths[STYLE_ALIGN_CENTRE];
	wr = widths[STYLE_ALIGN_RIGHT];
	wa = widths[STYLE_ALIGN_ABSOLUTE_CENTRE];
	while (wl + wc + wr > width) {
		if (wc > 0)
			wc--;
		else if (wr > 0)
			wr--;
		else
			wl--;
	}
	if (wa > width)
		wa = width;

	start[STYLE_ALIGN_LEFT] = 0;
	src[STYLE_ALIGN_LEFT] = 0;
	vis[STYLE_ALIGN_LEFT] = wl;

	start[STYLE_ALIGN_RIGHT] = width - wr;
	src[STYLE_ALIGN_RIGHT] = widths[STYLE_ALIGN_RIGHT] - wr;
	vis[STYLE_ALIGN_RIGHT] = wr;

	start[STYLE_ALIGN_CENTRE] =
	    wl + ((width - wr) - wl) / 2 - wc / 2;
	src[STYLE_ALIGN_CENTRE] = widths[STYLE_ALIGN_CENTRE] / 2 - wc / 2;
	vis[STYLE_ALIGN_CENTRE] = wc;

	start[STYLE_ALIGN_ABSOLUTE_CENTRE] = (width - wa) / 2;
	src[STYLE_ALIGN_ABSOLUTE_CENTRE] = 0;
	vis[STYLE_ALIGN_ABSOLUTE_CENTRE] = wa;

	/* Set a bit for each column of each matched character. */
	mask = bit_alloc(width);
	for (i = 0; i < ncs; i++) {
		if (!matched[i])
			continue;
		if (fuzzy_column(&cs[i], start, src, vis, &column) != 0)
			continue;
		for (j = 0; j < cs[i].width && column + j < width; j++)
			bit_set(mask, column + j);
	}

	free(matched);
	free(cs);

	if (score != NULL)
		*score = (total < 0) ? 0 : (u_int)total;
	return (mask);
}
