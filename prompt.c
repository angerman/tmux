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
#include <sys/time.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tmux.h"

struct prompt {
	char			*string;
	struct utf8_data	*buffer;
	struct cmd_find_state	 state;
	char			*last;
	size_t			 index;

	prompt_input_cb		 inputcb;
	prompt_free_cb		 freecb;
	void			*data;

	u_int			 hindex[PROMPT_NTYPES];
	struct utf8_data	*saved;
	int			 flags;
	enum prompt_type	 type;
	u_int			 menu_x;
	int			 closed;
};

struct prompt_menu {
	struct client	 *c;
	struct prompt	 *pr;
	u_int		  start;
	u_int		  size;
	char		**list;
};

static char	*prompt_complete(struct prompt *, struct client *, const char *,
		    u_int);

/* Create prompt. */
struct prompt *
prompt_create(struct client *c, const struct prompt_create_data *pd)
{
	struct prompt		*pr;
	struct format_tree	*ft;
	const char		*input = pd->input;
	char			*tmp;

	pr = xcalloc(1, sizeof *pr);

	if (pd->fs != NULL) {
		ft = format_create_from_state(NULL, c, pd->fs);
		cmd_find_copy_state(&pr->state, pd->fs);
	} else {
		ft = format_create_defaults(NULL, c, NULL, NULL, NULL);
		cmd_find_clear_state(&pr->state, 0);
	}

	if (input == NULL)
		input = "";

	pr->string = xstrdup(pd->prompt);
	if (pd->flags & PROMPT_NOFORMAT)
		tmp = xstrdup(input);
	else
		tmp = format_expand_time(ft, input);
	if (pd->flags & PROMPT_INCREMENTAL) {
		pr->last = xstrdup(tmp);
		pr->buffer = utf8_fromcstr("");
	} else {
		pr->last = NULL;
		pr->buffer = utf8_fromcstr(tmp);
	}
	pr->index = utf8_strlen(pr->buffer);
	free(tmp);

	pr->inputcb = pd->inputcb;
	pr->freecb = pd->freecb;
	pr->data = pd->data;

	pr->flags = pd->flags;
	pr->type = pd->type;

	format_free(ft);
	return (pr);
}

/* Free prompt. */
void
prompt_free(struct prompt *pr)
{
	if (pr != NULL) {
		if (pr->freecb != NULL && pr->data != NULL)
			pr->freecb(pr->data);
		free(pr->last);
		free(pr->string);
		free(pr->buffer);
		free(pr->saved);
		free(pr);
	}
}

/* Start prompt. */
void
prompt_start(struct prompt *pr, struct client *c)
{
	char	*tmp, *cp;

	if (pr->flags & PROMPT_INCREMENTAL) {
		tmp = utf8_tocstr(pr->buffer);
		xasprintf(&cp, "=%s", tmp);
		pr->inputcb(c, pr->data, cp, PROMPT_KEY_HANDLED);
		free(cp);
		free(tmp);
	}
}

/* Accept prompt. */
void
prompt_accept(struct prompt *pr, struct client *c, const char *s)
{
	pr->inputcb(c, pr->data, s, PROMPT_KEY_CLOSE);
	pr->closed = 1;
}

/* Update prompt. */
void
prompt_update(struct prompt *pr, struct client *c, const char *msg,
    const char *input)
{
	struct format_tree	*ft;
	char			*tmp;

	if (cmd_find_valid_state(&pr->state))
 		ft = format_create_from_state(NULL, c, &pr->state);
	else
 		ft = format_create_defaults(NULL, c, NULL, NULL, NULL);

	free(pr->string);
	pr->string = xstrdup(msg);

	free(pr->buffer);
	tmp = format_expand_time(ft, input);
	pr->buffer = utf8_fromcstr(tmp);
	pr->index = utf8_strlen(pr->buffer);
	free(tmp);

	memset(pr->hindex, 0, sizeof pr->hindex);
	pr->closed = 0;

	format_free(ft);
}

/* Is this prompt closed? */
int
prompt_closed(struct prompt *pr)
{
	return (pr->closed);
}

/* Does this prompt have this input callback? */
int
prompt_is_inputcb(struct prompt *pr, prompt_input_cb inputcb)
{
	if (pr == NULL)
		return (0);
	return (pr->inputcb == inputcb);
}

/* Redraw character. Return 1 if can continue redrawing, 0 otherwise. */
static int
prompt_redraw_character(struct screen_write_ctx *ctx, u_int offset,
    u_int pwidth, u_int *width, struct grid_cell *gc,
    const struct utf8_data *ud)
{
	u_char	ch;

	if (*width < offset) {
		*width += ud->width;
		return (1);
	}
	if (*width >= offset + pwidth)
		return (0);
	*width += ud->width;
	if (*width > offset + pwidth)
		return (0);

	ch = *ud->data;
	if (ud->size == 1 && (ch <= 0x1f || ch == 0x7f)) {
		gc->data.data[0] = '^';
		gc->data.data[1] = (ch == 0x7f) ? '?' : ch|0x40;
		gc->data.size = gc->data.have = 2;
		gc->data.width = 2;
	} else
		utf8_copy(&gc->data, ud);
	screen_write_cell(ctx, gc);
	return (1);
}

/*
 * Redraw quote indicator '^' if necessary. Return 1 if can continue redrawing,
 * 0 otherwise.
 */
static int
prompt_redraw_quote(const struct prompt *pr, u_int pcursor,
    struct screen_write_ctx *ctx, u_int offset, u_int pwidth, u_int *width,
    struct grid_cell *gc)
{
	struct utf8_data	ud;

	if (pr->flags & PROMPT_QUOTENEXT && ctx->s->cx == pcursor + 1) {
		utf8_set(&ud, '^');
		return (prompt_redraw_character(ctx, offset, pwidth,
		    width, gc, &ud));
	}
	return (1);
}

/* Draw prompt. */
void
prompt_draw(struct prompt *pr, struct client *c, struct screen_write_ctx *ctx,
    u_int ax, u_int py, u_int aw, u_int *cx)
{
	struct options 		*oo = c->session->options;
	struct screen		*s = ctx->s;
	struct format_tree	*ft;
	struct grid_cell	 gc;
	u_int			 i, offset, left, start, width, n;
	u_int			 pcursor, pwidth;
	const char		*msgfmt;
	char			*expanded, *prompt, *tmp;

	pr->menu_x = ax;

	/* Choose the cursor colour and style for this prompt. */
	n = options_get_number(oo, "prompt-cursor-colour");
	s->default_ccolour = n;
	if (pr->flags & PROMPT_COMMANDMODE) {
		n = options_get_number(oo, "prompt-command-cursor-style");
		style_apply(&gc, oo, "message-command-style", NULL);
	} else {
		n = options_get_number(oo, "prompt-cursor-style");
		style_apply(&gc, oo, "message-style", NULL);
	}
	screen_set_cursor_style(n, &s->default_cstyle, &s->default_mode);

	/* Expand the prompt itself. */
	if (cmd_find_valid_state(&pr->state))
 		ft = format_create_from_state(NULL, c, &pr->state);
	else
 		ft = format_create_defaults(NULL, c, NULL, NULL, NULL);
	tmp = utf8_tocstr(pr->buffer);
	format_add(ft, "prompt_input", "%s", tmp);
	prompt = format_expand_time(ft, pr->string);
	free(tmp);

	/*
	 * Set #{message} to the prompt string and expand message-format.
	 * format_draw handles fill, alignment, and decorations in one call.
	 */
	format_add(ft, "message", "%s", prompt);
	if (pr->flags & PROMPT_COMMANDMODE)
		format_add(ft, "command_prompt", "1");
	else
		format_add(ft, "command_prompt", "0");
	msgfmt = options_get_string(oo, "message-format");
	expanded = format_expand_time(ft, msgfmt);
	free(prompt);

	start = format_width(expanded);
	if (start > aw)
		start = aw;
	*cx = ax + start;

	screen_write_cursormove(ctx, ax, py, 0);
	format_draw(ctx, &gc, aw, expanded, NULL, 0);
	screen_write_cursormove(ctx, ax + start, py, 0);

	free(expanded);
	format_free(ft);

	left = aw - start;
	if (left == 0)
		return;

	pcursor = utf8_strwidth(pr->buffer, pr->index);
	pwidth = utf8_strwidth(pr->buffer, -1);
	if (pr->flags & PROMPT_QUOTENEXT)
		pwidth++;
	if (pcursor >= left) {
		/*
		 * The cursor would be outside the screen so start drawing
		 * with it on the right.
		 */
		offset = (pcursor - left) + 1;
		pwidth = left;
	} else
		offset = 0;
	if (pwidth > left)
		pwidth = left;
	*cx = ax + start + pcursor - offset;

	width = 0;
	for (i = 0; pr->buffer[i].size != 0; i++) {
		if (!prompt_redraw_quote(pr, pcursor, ctx, offset, pwidth,
		    &width, &gc))
			break;
		if (!prompt_redraw_character(ctx, offset, pwidth, &width, &gc,
		    &pr->buffer[i]))
			break;
	}
	prompt_redraw_quote(pr, pcursor, ctx, offset, pwidth, &width, &gc);
}

/* Is this a separator? */
static int
prompt_in_list(const char *ws, const struct utf8_data *ud)
{
	if (ud->size != 1 || ud->width != 1)
		return (0);
	return (strchr(ws, *ud->data) != NULL);
}

/* Is this a space? */
static int
prompt_space(const struct utf8_data *ud)
{
	if (ud->size != 1 || ud->width != 1)
		return (0);
	return (*ud->data == ' ');
}

/* Is this a keypad key? */
static key_code
prompt_keypad_key(key_code key)
{
	if (key & KEYC_MASK_MODIFIERS)
		return (key);

	switch (key) {
	case KEYC_KP_SLASH:
		return ('/');
	case KEYC_KP_STAR:
		return ('*');
	case KEYC_KP_MINUS:
		return ('-');
	case KEYC_KP_SEVEN:
		return ('7');
	case KEYC_KP_EIGHT:
		return ('8');
	case KEYC_KP_NINE:
		return ('9');
	case KEYC_KP_PLUS:
		return ('+');
	case KEYC_KP_FOUR:
		return ('4');
	case KEYC_KP_FIVE:
		return ('5');
	case KEYC_KP_SIX:
		return ('6');
	case KEYC_KP_ONE:
		return ('1');
	case KEYC_KP_TWO:
		return ('2');
	case KEYC_KP_THREE:
		return ('3');
	case KEYC_KP_ENTER:
		return ('\r');
	case KEYC_KP_ZERO:
		return ('0');
	case KEYC_KP_PERIOD:
		return ('.');
	}
	return (key);
}

/*
 * Translate key from vi to emacs. Return 0 to drop key, 1 to process the key
 * as an emacs key; return 2 to append to the buffer.
 */
static int
prompt_translate_key(struct prompt *pr, struct client *c, key_code key,
    key_code *new_key)
{
	if (~pr->flags & PROMPT_COMMANDMODE) {
		switch (key) {
		case 'a'|KEYC_CTRL:
		case 'c'|KEYC_CTRL:
		case 'e'|KEYC_CTRL:
		case 'g'|KEYC_CTRL:
		case 'h'|KEYC_CTRL:
		case '\011': /* Tab */
		case 'k'|KEYC_CTRL:
		case 'n'|KEYC_CTRL:
		case 'p'|KEYC_CTRL:
		case 't'|KEYC_CTRL:
		case 'u'|KEYC_CTRL:
		case 'v'|KEYC_CTRL:
		case 'w'|KEYC_CTRL:
		case 'y'|KEYC_CTRL:
		case '\n':
		case '\r':
		case KEYC_LEFT|KEYC_CTRL:
		case KEYC_RIGHT|KEYC_CTRL:
		case KEYC_BSPACE:
		case KEYC_DC:
		case KEYC_DOWN:
		case KEYC_END:
		case KEYC_HOME:
		case KEYC_LEFT:
		case KEYC_RIGHT:
		case KEYC_UP:
			*new_key = key;
			return (1);
		case '\033': /* Escape */
		case '['|KEYC_CTRL:
			pr->flags |= PROMPT_COMMANDMODE;
			if (pr->index != 0)
				pr->index--;
			c->flags |= CLIENT_REDRAWSTATUS;
			return (0);
		}
		*new_key = key;
		return (2);
	}

	switch (key) {
	case KEYC_BSPACE:
		*new_key = KEYC_LEFT;
		return (1);
	case 'A':
	case 'I':
	case 'C':
	case 's':
	case 'a':
		pr->flags &= ~PROMPT_COMMANDMODE;
		c->flags |= CLIENT_REDRAWSTATUS;
		break; /* switch mode and... */
	case 'S':
		pr->flags &= ~PROMPT_COMMANDMODE;
		c->flags |= CLIENT_REDRAWSTATUS;
		*new_key = 'u'|KEYC_CTRL;
		return (1);
	case 'i':
		pr->flags &= ~PROMPT_COMMANDMODE;
		c->flags |= CLIENT_REDRAWSTATUS;
		return (0);
	case '\033': /* Escape */
	case '['|KEYC_CTRL:
		return (0);
	}

	switch (key) {
	case 'A':
	case '$':
		*new_key = KEYC_END;
		return (1);
	case 'I':
	case '0':
	case '^':
		*new_key = KEYC_HOME;
		return (1);
	case 'C':
	case 'D':
		*new_key = 'k'|KEYC_CTRL;
		return (1);
	case KEYC_BSPACE:
	case 'X':
		*new_key = KEYC_BSPACE;
		return (1);
	case 'b':
		*new_key = 'b'|KEYC_META;
		return (1);
	case 'B':
		*new_key = 'B'|KEYC_VI;
		return (1);
	case 'd':
		*new_key = 'u'|KEYC_CTRL;
		return (1);
	case 'e':
		*new_key = 'e'|KEYC_VI;
		return (1);
	case 'E':
		*new_key = 'E'|KEYC_VI;
		return (1);
	case 'w':
		*new_key = 'w'|KEYC_VI;
		return (1);
	case 'W':
		*new_key = 'W'|KEYC_VI;
		return (1);
	case 'p':
		*new_key = 'y'|KEYC_CTRL;
		return (1);
	case 'q':
		*new_key = 'c'|KEYC_CTRL;
		return (1);
	case 's':
	case KEYC_DC:
	case 'x':
		*new_key = KEYC_DC;
		return (1);
	case KEYC_DOWN:
	case 'j':
		*new_key = KEYC_DOWN;
		return (1);
	case KEYC_LEFT:
	case 'h':
		*new_key = KEYC_LEFT;
		return (1);
	case 'a':
	case KEYC_RIGHT:
	case 'l':
		*new_key = KEYC_RIGHT;
		return (1);
	case KEYC_UP:
	case 'k':
		*new_key = KEYC_UP;
		return (1);
	case 'h'|KEYC_CTRL:
	case 'c'|KEYC_CTRL:
	case '\n':
	case '\r':
		return (1);
	}
	return (0);
}

/* Paste into prompt. */
static int
prompt_paste(struct prompt *pr)
{
	struct paste_buffer	*pb;
	const char		*bufdata;
	size_t			 size, n, bufsize;
	u_int			 i;
	struct utf8_data	*ud, *udp;
	enum utf8_state		 more;

	size = utf8_strlen(pr->buffer);
	if (pr->saved != NULL) {
		ud = pr->saved;
		n = utf8_strlen(pr->saved);
	} else {
		if ((pb = paste_get_top(NULL)) == NULL)
			return (0);
		bufdata = paste_buffer_data(pb, &bufsize);
		ud = udp = xreallocarray(NULL, bufsize + 1, sizeof *ud);
		for (i = 0; i != bufsize; /* nothing */) {
			more = utf8_open(udp, bufdata[i]);
			if (more == UTF8_MORE) {
				while (++i != bufsize && more == UTF8_MORE)
					more = utf8_append(udp, bufdata[i]);
				if (more == UTF8_DONE) {
					udp++;
					continue;
				}
				i -= udp->have;
			}
			if (bufdata[i] <= 31 || bufdata[i] >= 127)
				break;
			utf8_set(udp, bufdata[i]);
			udp++;
			i++;
		}
		udp->size = 0;
		n = udp - ud;
	}
	if (n != 0) {
		pr->buffer = xreallocarray(pr->buffer, size + n + 1,
		    sizeof *pr->buffer);
		if (pr->index == size) {
			memcpy(pr->buffer + pr->index, ud,
			    n * sizeof *pr->buffer);
			pr->index += n;
			pr->buffer[pr->index].size = 0;
		} else {
			memmove(pr->buffer + pr->index + n,
			    pr->buffer + pr->index,
			    (size + 1 - pr->index) *
			    sizeof *pr->buffer);
			memcpy(pr->buffer + pr->index, ud,
			    n * sizeof *pr->buffer);
			pr->index += n;
		}
	}
	if (ud != pr->saved)
		free(ud);
	return (1);
}

/* Finish completion. */
static int
prompt_replace_complete(struct prompt *pr, struct client *c, const char *s)
{
	char			 word[64], *allocated = NULL;
	size_t			 size, n, off, idx, used;
	struct utf8_data	*first, *last, *ud;

	/* Work out where the cursor currently is. */
	idx = pr->index;
	if (idx != 0)
		idx--;
	size = utf8_strlen(pr->buffer);

	/* Find the word we are in. */
	first = &pr->buffer[idx];
	while (first > pr->buffer && !prompt_space(first))
		first--;
	while (first->size != 0 && prompt_space(first))
		first++;
	last = &pr->buffer[idx];
	while (last->size != 0 && !prompt_space(last))
		last++;
	while (last > pr->buffer && prompt_space(last))
		last--;
	if (last->size != 0)
		last++;
	if (last < first)
		return (0);
	if (s == NULL) {
		used = 0;
		for (ud = first; ud < last; ud++) {
			if (used + ud->size >= sizeof word)
				break;
			memcpy(word + used, ud->data, ud->size);
			used += ud->size;
		}
		if (ud != last)
			return (0);
		word[used] = '\0';
	}

	/* Try to complete it. */
	if (s == NULL) {
		allocated = prompt_complete(pr, c, word,
		    first - pr->buffer);
		if (allocated == NULL)
			return (0);
		s = allocated;
	}

	/* Trim out word. */
	n = size - (last - pr->buffer) + 1; /* with \0 */
	memmove(first, last, n * sizeof *pr->buffer);
	size -= last - first;

	/* Insert the new word. */
	size += strlen(s);
	off = first - pr->buffer;
	pr->buffer = xreallocarray(pr->buffer, size + 1,
	    sizeof *pr->buffer);
	first = pr->buffer + off;
	memmove(first + strlen(s), first, n * sizeof *pr->buffer);
	for (idx = 0; idx < strlen(s); idx++)
		utf8_set(&first[idx], s[idx]);
	pr->index = (first - pr->buffer) + strlen(s);

	free(allocated);
	return (1);
}

/* Prompt forward to the next beginning of a word. */
static void
prompt_forward_word(struct prompt *pr, size_t size, int vi,
    const char *separators)
{
	size_t		 idx = pr->index;
	int		 word_is_separators;

	/* In emacs mode, skip until the first non-whitespace character. */
	if (!vi) {
		while (idx != size && prompt_space(&pr->buffer[idx]))
			idx++;
	}

	/* Can't move forward if we're already at the end. */
	if (idx == size) {
		pr->index = idx;
		return;
	}

	/* Determine the current character class (separators or not). */
	word_is_separators = prompt_in_list(separators, &pr->buffer[idx]) &&
	    !prompt_space(&pr->buffer[idx]);

	/* Skip ahead until the first space or opposite character class. */
	do {
		idx++;
		if (prompt_space(&pr->buffer[idx])) {
			/* In vi mode, go to the start of the next word. */
			if (vi) {
				while (idx != size &&
				    prompt_space(&pr->buffer[idx]))
					idx++;
			}
			break;
		}
	} while (idx != size && word_is_separators == prompt_in_list(
	    separators, &pr->buffer[idx]));

	pr->index = idx;
}

/* Prompt forward to the next end of a word. */
static void
prompt_end_word(struct prompt *pr, size_t size, const char *separators)
{
	size_t		 idx = pr->index;
	int		 word_is_separators;

	/* Can't move forward if we're already at the end. */
	if (idx == size)
		return;

	/* Find the next word. */
	do {
		idx++;
		if (idx == size) {
			pr->index = idx;
			return;
		}
	} while (prompt_space(&pr->buffer[idx]));

	/* Determine the character class (separators or not). */
	word_is_separators = prompt_in_list(separators,
	    &pr->buffer[idx]);

	/* Skip ahead until the next space or opposite character class. */
	do {
		idx++;
		if (idx == size)
			break;
	} while (!prompt_space(&pr->buffer[idx]) &&
	    word_is_separators == prompt_in_list(separators, &pr->buffer[idx]));

	/* Back up to the previous character to stop at the end of the word. */
	pr->index = idx - 1;
}

/* Prompt backward to the previous beginning of a word. */
static void
prompt_backward_word(struct prompt *pr, const char *separators)
{
	size_t	idx = pr->index;
	int	word_is_separators;

	/* Find non-whitespace. */
	while (idx != 0) {
		--idx;
		if (!prompt_space(&pr->buffer[idx]))
			break;
	}
	word_is_separators = prompt_in_list(separators,
	    &pr->buffer[idx]);

	/* Find the character before the beginning of the word. */
	while (idx != 0) {
		--idx;
		if (prompt_space(&pr->buffer[idx]) ||
		    word_is_separators != prompt_in_list(separators,
		    &pr->buffer[idx])) {
			/* Go back to the word. */
			idx++;
			break;
		}
	}
	pr->index = idx;
}

/* Fire input callback when done. */
static enum prompt_key_result
prompt_done(struct prompt *pr, struct client *c, const char *s)
{
	void	*pd = pr->data;

	if (pr->inputcb(c, pd, s, PROMPT_KEY_CLOSE) == PROMPT_CLOSE) {
		pr->closed = 1;
		return (PROMPT_KEY_CLOSE);
	}
	c->flags |= CLIENT_REDRAWSTATUS;
	return (PROMPT_KEY_HANDLED);
}

/* Check for a movement key. */
static enum prompt_key_result
prompt_check_move(struct prompt *pr, struct client *c, key_code key)
{
	void	*pd = pr->data;
	char	*s;

	if (~pr->flags & PROMPT_INCREMENTAL)
		return (PROMPT_KEY_NOT_HANDLED);
	switch (key) {
	case KEYC_UP:
	case KEYC_DOWN:
	case KEYC_LEFT:
	case KEYC_RIGHT:
	case KEYC_PPAGE:
	case KEYC_NPAGE:
		break;
	default:
		return (PROMPT_KEY_NOT_HANDLED);
	}
	s = utf8_tocstr(pr->buffer);
	if (pr->inputcb(c, pd, s, PROMPT_KEY_MOVE) == PROMPT_CLOSE) {
		pr->closed = 1;
		free(s);
		return (PROMPT_KEY_CLOSE);
	}
	free(s);
	return (PROMPT_KEY_MOVE);
}

/* Handle keys in prompt. */
enum prompt_key_result
prompt_key(struct prompt *pr, struct client *c, key_code key)
{
	void			*pd = pr->data;
	struct options		*oo = c->session->options;
	char			*s, *cp, prefix = '=';
	const char		*histstr, *separators = NULL, *ks;
	size_t			 size, idx;
	struct utf8_data	 tmp;
	enum prompt_key_result	 result = PROMPT_KEY_HANDLED;
	int			 keys, word_is_separators;

	pr->closed = 0;
	if (pr->flags & PROMPT_KEY) {
		ks = key_string_lookup_key(key, 0);
		pr->inputcb(c, pd, ks, PROMPT_KEY_CLOSE);
		pr->closed = 1;
		return (PROMPT_KEY_CLOSE);
	}
	size = utf8_strlen(pr->buffer);

	key &= ~KEYC_MASK_FLAGS;
	key = prompt_keypad_key(key);

	if (pr->flags & PROMPT_NUMERIC) {
		if (key >= '0' && key <= '9')
			goto append_key;
		s = utf8_tocstr(pr->buffer);
		pr->inputcb(c, pd, s, PROMPT_KEY_CLOSE);
		pr->closed = 1;
		free(s);
		return (PROMPT_KEY_NOT_HANDLED);
	}

	if (pr->flags & (PROMPT_SINGLE|PROMPT_QUOTENEXT)) {
		if ((key & KEYC_MASK_KEY) == KEYC_BSPACE)
			key = 0x7f;
		else if ((key & KEYC_MASK_KEY) > 0x7f) {
			if (!KEYC_IS_UNICODE(key))
				return (PROMPT_KEY_HANDLED);
			key &= KEYC_MASK_KEY;
		} else
			key &= (key & KEYC_CTRL) ? 0x1f : KEYC_MASK_KEY;
		pr->flags &= ~PROMPT_QUOTENEXT;
		goto append_key;
	}

	keys = options_get_number(c->session->options, "status-keys");
	if (keys == MODEKEY_VI) {
		switch (prompt_translate_key(pr, c, key, &key)) {
		case 1:
			goto process_key;
		case 2:
			goto append_key;
		default:
			return (PROMPT_KEY_HANDLED);
		}
	}

process_key:
	result = prompt_check_move(pr, c, key);
	if (result != PROMPT_KEY_NOT_HANDLED)
		return (result);
	switch (key) {
	case KEYC_LEFT:
	case 'b'|KEYC_CTRL:
		if (pr->index > 0) {
			pr->index--;
			break;
		}
		break;
	case KEYC_RIGHT:
	case 'f'|KEYC_CTRL:
		if (pr->index < size) {
			pr->index++;
			break;
		}
		break;
	case KEYC_HOME:
	case 'a'|KEYC_CTRL:
		if (pr->index != 0) {
			pr->index = 0;
			break;
		}
		break;
	case KEYC_END:
	case 'e'|KEYC_CTRL:
		if (pr->index != size) {
			pr->index = size;
			break;
		}
		break;
	case '\011': /* Tab */
		if (prompt_replace_complete(pr, c, NULL))
			goto changed;
		break;
	case KEYC_BSPACE:
	case 'h'|KEYC_CTRL:
		if (pr->flags & PROMPT_BSPACE_EXIT && size == 0)
			return (prompt_done(pr, c, NULL));
		if (pr->index != 0) {
			if (pr->index == size)
				pr->buffer[--pr->index].size = 0;
			else {
				memmove(pr->buffer + pr->index - 1,
				    pr->buffer + pr->index,
				    (size + 1 - pr->index) *
				    sizeof *pr->buffer);
				pr->index--;
			}
			goto changed;
		}
		break;
	case KEYC_DC:
	case 'd'|KEYC_CTRL:
		if (pr->index != size) {
			memmove(pr->buffer + pr->index,
			    pr->buffer + pr->index + 1,
			    (size + 1 - pr->index) *
			    sizeof *pr->buffer);
			goto changed;
		}
		break;
	case 'u'|KEYC_CTRL:
		pr->buffer[0].size = 0;
		pr->index = 0;
		goto changed;
	case 'k'|KEYC_CTRL:
		if (pr->index < size) {
			pr->buffer[pr->index].size = 0;
			goto changed;
		}
		break;
	case 'w'|KEYC_CTRL:
		separators = options_get_string(oo, "word-separators");
		idx = pr->index;

		/* Find non-whitespace. */
		while (idx != 0) {
			idx--;
			if (!prompt_space(&pr->buffer[idx]))
				break;
		}
		word_is_separators = prompt_in_list(separators,
		    &pr->buffer[idx]);

		/* Find the character before the beginning of the word. */
		while (idx != 0) {
			idx--;
			if (prompt_space(&pr->buffer[idx]) ||
			    word_is_separators != prompt_in_list(
			    separators, &pr->buffer[idx])) {
				/* Go back to the word. */
				idx++;
				break;
			}
		}

		free(pr->saved);
		pr->saved = xcalloc(sizeof *pr->buffer,
		    (pr->index - idx) + 1);
		memcpy(pr->saved, pr->buffer + idx,
		    (pr->index - idx) * sizeof *pr->buffer);

		memmove(pr->buffer + idx, pr->buffer + pr->index,
		    (size + 1 - pr->index) * sizeof *pr->buffer);
		memset(pr->buffer + size - (pr->index - idx), '\0',
		    (pr->index - idx) * sizeof *pr->buffer);
		pr->index = idx;

		goto changed;
	case KEYC_RIGHT|KEYC_CTRL:
	case 'f'|KEYC_META:
		separators = options_get_string(oo, "word-separators");
		prompt_forward_word(pr, size, 0, separators);
		goto changed;
	case 'E'|KEYC_VI:
		prompt_end_word(pr, size, "");
		goto changed;
	case 'e'|KEYC_VI:
		separators = options_get_string(oo, "word-separators");
		prompt_end_word(pr, size, separators);
		goto changed;
	case 'W'|KEYC_VI:
		prompt_forward_word(pr, size, 1, "");
		goto changed;
	case 'w'|KEYC_VI:
		separators = options_get_string(oo, "word-separators");
		prompt_forward_word(pr, size, 1, separators);
		goto changed;
	case 'B'|KEYC_VI:
		prompt_backward_word(pr, "");
		goto changed;
	case KEYC_LEFT|KEYC_CTRL:
	case 'b'|KEYC_META:
		separators = options_get_string(oo, "word-separators");
		prompt_backward_word(pr, separators);
		goto changed;
	case KEYC_UP:
	case 'p'|KEYC_CTRL:
		histstr = prompt_up_history(pr->hindex,
		    pr->type);
		if (histstr == NULL)
			break;
		free(pr->buffer);
		pr->buffer = utf8_fromcstr(histstr);
		pr->index = utf8_strlen(pr->buffer);
		goto changed;
	case KEYC_DOWN:
	case 'n'|KEYC_CTRL:
		histstr = prompt_down_history(pr->hindex, pr->type);
		if (histstr == NULL)
			break;
		free(pr->buffer);
		pr->buffer = utf8_fromcstr(histstr);
		pr->index = utf8_strlen(pr->buffer);
		goto changed;
	case 'y'|KEYC_CTRL:
		if (prompt_paste(pr))
			goto changed;
		break;
	case 't'|KEYC_CTRL:
		idx = pr->index;
		if (idx < size)
			idx++;
		if (idx >= 2) {
			utf8_copy(&tmp, &pr->buffer[idx - 2]);
			utf8_copy(&pr->buffer[idx - 2], &pr->buffer[idx - 1]);
			utf8_copy(&pr->buffer[idx - 1], &tmp);
			pr->index = idx;
			goto changed;
		}
		break;
	case '\r':
	case '\n':
		s = utf8_tocstr(pr->buffer);
		if (*s != '\0')
			prompt_add_history(s, pr->type);
		result = prompt_done(pr, c, s);
		free(s);
		return (result);
	case '\033': /* Escape */
	case '['|KEYC_CTRL:
	case 'c'|KEYC_CTRL:
	case 'g'|KEYC_CTRL:
		return (prompt_done(pr, c, NULL));
	case 'r'|KEYC_CTRL:
		if (~pr->flags & PROMPT_INCREMENTAL)
			break;
		if (pr->buffer[0].size == 0) {
			prefix = '=';
			free(pr->buffer);
			pr->buffer = utf8_fromcstr(pr->last);
			pr->index = utf8_strlen(pr->buffer);
		} else
			prefix = '-';
		goto changed;
	case 's'|KEYC_CTRL:
		if (~pr->flags & PROMPT_INCREMENTAL)
			break;
		if (pr->buffer[0].size == 0) {
			prefix = '=';
			free(pr->buffer);
			pr->buffer = utf8_fromcstr(pr->last);
			pr->index = utf8_strlen(pr->buffer);
		} else
			prefix = '+';
		goto changed;
	case 'v'|KEYC_CTRL:
		pr->flags |= PROMPT_QUOTENEXT;
		break;
	default:
		goto append_key;
	}

	c->flags |= CLIENT_REDRAWSTATUS;
	return (PROMPT_KEY_HANDLED);

append_key:
	if (key <= 0x7f) {
		utf8_set(&tmp, key);
		if (key <= 0x1f || key == 0x7f)
			tmp.width = 2;
	} else if (KEYC_IS_UNICODE(key))
		utf8_to_data(key, &tmp);
	else
		return (PROMPT_KEY_HANDLED);

	pr->buffer = xreallocarray(pr->buffer, size + 2,
	    sizeof *pr->buffer);

	if (pr->index == size) {
		utf8_copy(&pr->buffer[pr->index], &tmp);
		pr->index++;
		pr->buffer[pr->index].size = 0;
	} else {
		memmove(pr->buffer + pr->index + 1,
		    pr->buffer + pr->index,
		    (size + 1 - pr->index) *
		    sizeof *pr->buffer);
		utf8_copy(&pr->buffer[pr->index], &tmp);
		pr->index++;
	}

	if (pr->flags & PROMPT_SINGLE) {
		if (utf8_strlen(pr->buffer) != 1) {
			pr->closed = 1;
			result = PROMPT_KEY_CLOSE;
		} else {
			s = utf8_tocstr(pr->buffer);
			result = prompt_done(pr, c, s);
			free(s);
		}
	}

changed:
	c->flags |= CLIENT_REDRAWSTATUS;
	if (pr->flags & PROMPT_INCREMENTAL) {
		s = utf8_tocstr(pr->buffer);
		xasprintf(&cp, "%c%s", prefix, s);
		pr->inputcb(c, pd, cp, PROMPT_KEY_HANDLED);
		free(cp);
		free(s);
	}
	return (result);
}

/* Add to completion list. */
static void
prompt_add_list(char ***list, u_int *size, const char *s)
{
	u_int	i;

	for (i = 0; i < *size; i++) {
		if (strcmp((*list)[i], s) == 0)
			return;
	}
	*list = xreallocarray(*list, (*size) + 1, sizeof **list);
	(*list)[(*size)++] = xstrdup(s);
}

/* Build completion list. */
static char **
prompt_complete_list(u_int *size, const char *s)
{
	char				**list = NULL, *tmp;
	const char			*value, *cp;
	const struct cmd_entry		**cmdent;
	size_t				 slen = strlen(s), valuelen;
	struct options_entry		*o;
	struct options_array_item	*a;

	*size = 0;
	for (cmdent = cmd_table; *cmdent != NULL; cmdent++) {
		if (strncmp((*cmdent)->name, s, slen) == 0)
			prompt_add_list(&list, size, (*cmdent)->name);
		if ((*cmdent)->alias != NULL &&
		    strncmp((*cmdent)->alias, s, slen) == 0)
			prompt_add_list(&list, size, (*cmdent)->alias);
	}
	o = options_get_only(global_options, "command-alias");
	if (o != NULL) {
		a = options_array_first(o);
		while (a != NULL) {
			value = options_array_item_value(a)->string;
			if ((cp = strchr(value, '=')) == NULL)
				goto next;
			valuelen = cp - value;
			if (slen > valuelen || strncmp(value, s, slen) != 0)
				goto next;

			xasprintf(&tmp, "%.*s", (int)valuelen, value);
			prompt_add_list(&list, size, tmp);
			free(tmp);

		next:
			a = options_array_next(a);
		}
	}
	return (list);
}

/* Find longest prefix. */
static char *
prompt_complete_prefix(char **list, u_int size)
{
	char	 *out;
	u_int	  i;
	size_t	  j;

	if (list == NULL || size == 0)
		return (NULL);
	out = xstrdup(list[0]);
	for (i = 1; i < size; i++) {
		j = strlen(list[i]);
		if (j > strlen(out))
			j = strlen(out);
		for (; j > 0; j--) {
			if (out[j - 1] != list[i][j - 1])
				out[j - 1] = '\0';
		}
	}
	return (out);
}

/* Complete word menu callback. */
static void
prompt_menu_callback(__unused struct menu *menu, u_int idx, key_code key,
    void *data)
{
	struct prompt_menu	*spm = data;
	struct prompt		*pr = spm->pr;
	struct client		*c = spm->c;
	u_int			 i;

	if (key != KEYC_NONE) {
		idx += spm->start;
		if (prompt_replace_complete(pr, c, spm->list[idx]))
			c->flags |= CLIENT_REDRAWSTATUS;
	}

	for (i = 0; i < spm->size; i++)
		free(spm->list[i]);
	free(spm->list);
}

/* Show complete word menu. */
static int
prompt_complete_list_menu(struct prompt *pr, struct client *c, char **list,
    u_int size, u_int offset)
{
	struct menu		*menu;
	struct menu_item	 item;
	struct prompt_menu	*spm;
	u_int			 lines = status_line_size(c), height, i, py;

	if (size <= 1)
		return (0);
	if (c->tty.sy - lines < 3)
		return (0);

	spm = xmalloc(sizeof *spm);
	spm->c = c;
	spm->pr = pr;
	spm->size = size;
	spm->list = list;

	height = c->tty.sy - lines - 2;
	if (height > 10)
		height = 10;
	if (height > size)
		height = size;
	spm->start = size - height;

	menu = menu_create("");
	for (i = spm->start; i < size; i++) {
		item.name = list[i];
		item.key = '0' + (i - spm->start);
		item.command = NULL;
		menu_add_item(menu, &item, NULL, c, NULL);
	}

	if (options_get_number(c->session->options, "status-position") == 0)
		py = lines;
	else
		py = c->tty.sy - 3 - height;
	offset += utf8_cstrwidth(pr->string);
	offset += pr->menu_x;
	if (offset > 2)
		offset -= 2;
	else
		offset = 0;

	if (menu_display(menu, MENU_NOMOUSE|MENU_TAB, 0, NULL, offset, py, c,
	    BOX_LINES_DEFAULT, NULL, NULL, NULL, NULL,
	    prompt_menu_callback, spm) != 0) {
		menu_free(menu);
		free(spm);
		return (0);
	}
	return (1);
}

/* Sort complete list. */
static int
prompt_complete_sort(const void *a, const void *b)
{
	const char	**aa = (const char **)a, **bb = (const char **)b;

	return (strcmp(*aa, *bb));
}

/* Complete word. */
static char *
prompt_complete(struct prompt *pr, struct client *c, const char *word,
    u_int offset)
{
	char	**list = NULL, *out = NULL;
	u_int	  size = 0, i;

	if (pr->type != PROMPT_TYPE_COMMAND || offset != 0 ||
	    *word == '\0')
		return (NULL);

	list = prompt_complete_list(&size, word);
	if (size == 0)
		out = NULL;
	else if (size == 1)
		xasprintf(&out, "%s ", list[0]);
	else
		out = prompt_complete_prefix(list, size);

	if (size != 0) {
		qsort(list, size, sizeof *list, prompt_complete_sort);
		for (i = 0; i < size; i++)
			log_debug("complete %u: %s", i, list[i]);
	}

	if (out != NULL && strcmp(word, out) == 0) {
		free(out);
		out = NULL;
	}
	if (out != NULL ||
	    !prompt_complete_list_menu(pr, c, list, size, offset)) {
		for (i = 0; i < size; i++)
			free(list[i]);
		free(list);
	}
	return (out);
}


/* Return the type of the prompt as an enum. */
enum prompt_type
prompt_type(const char *type)
{
	u_int	i;

	for (i = 0; i < PROMPT_NTYPES; i++) {
		if (strcmp(type, prompt_type_string(i)) == 0)
			return (i);
	}
	return (PROMPT_TYPE_INVALID);
}

/* Get prompt type as a string. */
const char *
prompt_type_string(enum prompt_type type)
{
	switch (type) {
	case PROMPT_TYPE_COMMAND:
		return ("command");
	case PROMPT_TYPE_SEARCH:
		return ("search");
	case PROMPT_TYPE_INVALID:
		return ("invalid");
	}
	return ("unknown");
}
