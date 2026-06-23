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
#include <string.h>

#include "tmux.h"

/*
 * Fuzzy subsequence matching in the style of fzf. The pattern is split on
 * spaces into tokens and every token must match as a subsequence (its
 * characters appearing in order, not necessarily consecutively) somewhere in
 * the text, so "foo bar" requires both "foo" and "bar" to be present. Each
 * token is matched greedily and independently from the start of the text.
 *
 * In addition to the yes/no result, an optional score is produced from cheap
 * fzf-style bonuses (matches at the string start, after word boundaries and in
 * contiguous runs score higher) so that callers can rank best-match-first; and
 * the matched character indices may optionally be returned for highlighting.
 */

#define FUZZY_BONUS_START 12
#define FUZZY_BONUS_BOUNDARY 8
#define FUZZY_BONUS_CONSECUTIVE 6
#define FUZZY_PENALTY_LEADING 1
#define FUZZY_PENALTY_LEADING_MAX 10

/* Is this character a word boundary, so a match after it scores higher? */
static int
fuzzy_is_boundary(u_char c)
{
	return (c == ' ' || c == '-' || c == '_' || c == '/' || c == '.' ||
	    c == ':');
}

/* Compare two characters, optionally ignoring case. */
static int
fuzzy_equal(u_char a, u_char b, int icase)
{
	if (icase)
		return (tolower(a) == tolower(b));
	return (a == b);
}

/*
 * Match a single token (no spaces) as a subsequence of text, accumulating the
 * score and, if positions is not NULL, the matched indices. Returns 1 if the
 * token matches and 0 if not.
 */
static int
fuzzy_match_token(const char *pattern, size_t patternlen, const char *text,
    int icase, int *score, u_int *positions, u_int *npositions)
{
	const char	*end = pattern + patternlen;
	u_int		 ti = 0, last = 0;
	int		 started = 0;

	while (pattern != end) {
		while (text[ti] != '\0' &&
		    !fuzzy_equal(*pattern, text[ti], icase))
			ti++;
		if (text[ti] == '\0')
			return (0);

		if (score != NULL) {
			if (!started) {
				if (ti == 0)
					*score += FUZZY_BONUS_START;
				else {
					if (fuzzy_is_boundary(text[ti - 1]))
						*score += FUZZY_BONUS_BOUNDARY;
					if (ti < FUZZY_PENALTY_LEADING_MAX)
						*score -= ti * FUZZY_PENALTY_LEADING;
					else {
						*score -= FUZZY_PENALTY_LEADING_MAX *
						    FUZZY_PENALTY_LEADING;
					}
				}
				started = 1;
			} else {
				if (ti == last + 1)
					*score += FUZZY_BONUS_CONSECUTIVE;
				else if (fuzzy_is_boundary(text[ti - 1]))
					*score += FUZZY_BONUS_BOUNDARY;
			}
			last = ti;
		}

		if (positions != NULL)
			positions[(*npositions)++] = ti;

		pattern++;
		ti++;
	}
	return (1);
}

/*
 * Fuzzy match pattern against text. Returns 1 on match and 0 otherwise. If
 * score is not NULL it is set to the total match score (higher is better;
 * unspecified on no match). If positions is not NULL it must point to a buffer
 * of at least strlen(pattern) entries and is filled with the matched character
 * indices; npositions, which must then also be given, receives the count.
 */
int
fuzzy_match(const char *pattern, const char *text, int icase, int *score,
    u_int *positions, u_int *npositions)
{
	const char	*start;
	size_t		 len;

	if (score != NULL)
		*score = 0;
	if (npositions != NULL)
		*npositions = 0;

	while (*pattern != '\0') {
		while (*pattern == ' ')
			pattern++;
		if (*pattern == '\0')
			break;
		start = pattern;
		while (*pattern != '\0' && *pattern != ' ')
			pattern++;
		len = pattern - start;
		if (!fuzzy_match_token(start, len, text, icase, score,
		    positions, npositions))
			return (0);
	}
	return (1);
}
