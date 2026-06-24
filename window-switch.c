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

#include <stdlib.h>
#include <string.h>

#include "tmux.h"

static struct screen	*window_switch_init(struct window_mode_entry *,
			     struct cmd_find_state *, struct args *);
static void		 window_switch_free(struct window_mode_entry *);
static void		 window_switch_resize(struct window_mode_entry *, u_int,
			     u_int);
static void		 window_switch_key(struct window_mode_entry *,
			     struct client *, struct session *,
			     struct winlink *, key_code, struct mouse_event *);

#define WINDOW_SWITCH_DEFAULT_COMMAND "switch-client -Zt '%%'"

#define WINDOW_SWITCH_DEFAULT_FORMAT \
	"#{?window_format," \
		"#{window_name} #[dim](#{session_name})#[align=right]#{pane_current_command}" \
	"," \
		"#{session_name}" \
	"}"

const struct window_mode window_switch_mode = {
	.name = "switch-mode",
	.default_format = WINDOW_SWITCH_DEFAULT_FORMAT,

	.init = window_switch_init,
	.free = window_switch_free,
	.resize = window_switch_resize,
	.key = window_switch_key,
};

enum window_switch_type {
	WINDOW_SWITCH_TYPE_SESSION,
	WINDOW_SWITCH_TYPE_WINDOW
};

struct window_switch_itemdata {
	enum window_switch_type	 type;
	int			 session;
	int			 winlink;

	uint64_t		 tag;
	char			*text;
	bitstr_t		*match;

	int			 score;
	u_int			 order;
};

struct window_switch_modedata {
	struct window_pane		 *wp;
	struct screen		  	  screen;
	int				  zoomed;

	char				 *format;
	char				 *command;

	enum window_switch_type		  type;
	char				 *filter;

	struct window_switch_itemdata	**item_list;
	u_int				  item_size;

	struct window_switch_itemdata	**matches;
	u_int				  matches_size;
};

static void
window_switch_free_item(struct window_switch_itemdata *item)
{
	free(item->match);
	free(item->text);
	free(item);
}

static struct window_switch_itemdata *
window_switch_add_item(struct window_switch_modedata *data)
{
	struct window_switch_itemdata	*item;

	data->item_list = xreallocarray(data->item_list, data->item_size + 1,
	    sizeof *data->item_list);
	item = data->item_list[data->item_size++] = xcalloc(1, sizeof *item);
	return (item);
}

static void
window_switch_add_session(struct window_switch_modedata *data,
    struct session *s, u_int *order)
{
	struct window_switch_itemdata	*item;
	struct format_tree		*ft;

	ft = format_create(NULL, NULL, FORMAT_NONE, 0);
	format_defaults(ft, NULL, s, NULL, NULL);

	item = window_switch_add_item(data);
	item->type = WINDOW_SWITCH_TYPE_SESSION;
	item->session = s->id;
	item->winlink = -1;
	item->tag = (uint64_t)s;
	item->order = (*order)++;
	item->text = format_expand(ft, data->format);

	format_free(ft);
}

static void
window_switch_add_window(struct window_switch_modedata *data,
    struct winlink *wl, u_int *order)
{
	struct window_switch_itemdata	*item;
	struct format_tree		*ft;

	ft = format_create(NULL, NULL, FORMAT_NONE, 0);
	format_defaults(ft, NULL, wl->session, wl, NULL);

	item = window_switch_add_item(data);
	item->type = WINDOW_SWITCH_TYPE_WINDOW;
	item->session = wl->session->id;
	item->winlink = wl->idx;
	item->tag = (uint64_t)wl;
	item->order = (*order)++;
	item->text = format_expand(ft, data->format);

	format_free(ft);
}

static int
window_switch_compare(const void *a0, const void *b0)
{
	struct window_switch_itemdata	*const *a = a0;
	struct window_switch_itemdata	*const *b = b0;

	if ((*a)->score < (*b)->score)
		return (-1);
	if ((*a)->score > (*b)->score)
		return (1);
	if ((*a)->order < (*b)->order)
		return (-1);
	if ((*a)->order > (*b)->order)
		return (1);
	return (0);
}

static void
window_switch_build(struct window_switch_modedata *data)
{
	struct window_switch_itemdata	 *item, **m = NULL;
	const char			 *f = data->filter;
	u_int				  ns, nw, i, n = 0, order = 0, *p, np;
	u_int				  sx = screen_size_x(&data->screen);
	struct session			**sl;
	struct winlink			**wl;
	struct sort_criteria		  sort_crit;

	sort_crit.order = SORT_NAME;
	sort_crit.reversed = 0;

	for (i = 0; i < data->item_size; i++)
		window_switch_free_item(data->item_list[i]);
	free(data->item_list);
	data->item_list = NULL;
	data->item_size = 0;

	switch (data->type) {
	case WINDOW_SWITCH_TYPE_SESSION:
		sl = sort_get_sessions(&ns, &sort_crit);
		for (i = 0; i < ns; i++)
			window_switch_add_session(data, sl[i], &order);
		break;
	case WINDOW_SWITCH_TYPE_WINDOW:
		wl = sort_get_winlinks(&nw, &sort_crit);
		for (i = 0; i < nw; i++)
			window_switch_add_window(data, wl[i], &order);
		break;
	}

	for (i = 0; i < data->item_size; i++) {
		item = data->item_list[i];
		if (*f == '\0') {
			m = xreallocarray(m, n + 1, sizeof *m);
			m[n++] = item;
			continue;
		}

		item->match = fuzzy_match(f, item->text, sx, &item->score);
		if (item->match == NULL)
			continue;
		m = xreallocarray(m, n + 1, sizeof *m);
		m[n++] = item;
	}
	qsort(m, n, sizeof *m, window_switch_compare);

	free(data->matches);
	data->matches = m;
	data->matches_size = n;
}

static void
window_switch_draw_screen(struct window_mode_entry *wme)
{
	struct window_pane		*wp = wme->wp;
	struct window_switch_modedata	*data = wme->data;
	struct options			*oo = wp->options;
	struct screen_write_ctx		 ctx;
	struct screen			*s = &data->screen;
	u_int				 sx = screen_size_x(s), i, j, width;
	u_int				 sy = screen_size_y(s);
	struct window_switch_itemdata	*item;
	struct format_tree		*ft;
	const char			*format;
	char				*expanded;
	struct grid_cell		 mgc, gc;

	screen_write_start(&ctx, s);
	screen_write_clearscreen(&ctx, 8);

	if (sy <= 1) {
		screen_write_stop(&ctx);
		return;
	}

	style_apply(&mgc, oo, "switch-mode-match-style", NULL);

	for (i = 0; i < data->matches_size; i++) {
		if (i == sy - 1)
			break;
		item = data->matches[i];

		screen_write_cursormove(&ctx, 0, i, 0);
		format_draw(&ctx, &grid_default_cell, sx, item->text, NULL, 0);

		if (item->match == NULL)
			continue;
		for (j = 0; j < sx; j++) {
			if (!bit_test(item->match, j))
				continue;
			grid_get_cell(s->grid, j, i, &gc);
			gc.attr = mgc.attr;
			gc.fg = mgc.fg;
			gc.bg = mgc.bg;
			screen_write_cursormove(&ctx, j, i, 0);
			screen_write_cell(&ctx, &gc);
		}
	}

	ft = format_create(NULL, NULL, FORMAT_NONE, 0);
	format_add(ft, "filter", "%s", data->filter);
	format = options_get_string(oo, "switch-mode-filter-format");
	expanded = format_expand(ft, format);

	screen_write_cursormove(&ctx, 0, sy - 1, 0);
	width = format_width(expanded);
	format_draw(&ctx, &grid_default_cell, sx, expanded, NULL, 0);
	free(expanded);
	format_free(ft);

	if (width >= sx) {
		s->mode &= ~MODE_CURSOR;
		width = 0;
	}
	screen_write_cursormove(&ctx, width, sy - 1, 0);
}

static struct screen *
window_switch_init(struct window_mode_entry *wme,
    __unused struct cmd_find_state *fs, struct args *args)
{
	struct window_pane		*wp = wme->wp;
	struct window_switch_modedata	*data;
	struct screen			*s;

	wme->data = data = xcalloc(1, sizeof *data);
	data->wp = wp;
	data->filter = xstrdup("");

	if (args_has(args, 'w'))
		data->type = WINDOW_SWITCH_TYPE_WINDOW;
	else
		data->type = WINDOW_SWITCH_TYPE_SESSION;

	if (args == NULL || !args_has(args, 'F'))
		data->format = xstrdup(WINDOW_SWITCH_DEFAULT_FORMAT);
	else
		data->format = xstrdup(args_get(args, 'F'));
	if (args == NULL || args_count(args) == 0)
		data->command = xstrdup(WINDOW_SWITCH_DEFAULT_COMMAND);
	else
		data->command = xstrdup(args_string(args, 0));

	if (!args_has(args, 'Z'))
		data->zoomed = -1;
	else {
		data->zoomed = (wp->window->flags & WINDOW_ZOOMED);
		if (!data->zoomed && window_zoom(wp) == 0)
			server_redraw_window(wp->window);
	}

	s = &data->screen;
	screen_init(s, screen_size_x(&wp->base), screen_size_y(&wp->base), 0);

	window_switch_build(data);
	window_switch_draw_screen(wme);

	return (s);
}

static void
window_switch_free(struct window_mode_entry *wme)
{
	struct window_switch_modedata	*data = wme->data;
	u_int				 i;

	for (i = 0; i < data->item_size; i++)
		window_switch_free_item(data->item_list[i]);
	free(data->item_list);

	free(data->matches);
	free(data->filter);
	free(data->format);
	free(data->command);
	screen_free(&data->screen);

	free(data);
}

static void
window_switch_resize(struct window_mode_entry *wme, u_int sx, u_int sy)
{
	struct window_switch_modedata	*data = wme->data;
	struct screen			*s = &data->screen;

	screen_resize(s, sx, sy, 0);
	window_switch_draw_screen(wme);
}

static void
window_switch_select_current(struct window_switch_modedata *data,
    struct client *c)
{
#if 0
	struct window_switch_itemdata	*item;
	struct cmd_find_state		 fs;
	struct session			*s;
	struct winlink			*wl;
	char				*target = NULL;

	item = mode_tree_get_current(mtd);
	if (item == NULL)
		return;

	cmd_find_clear_state(&fs, 0);
	switch (item->type) {
	case WINDOW_SWITCH_TYPE_SESSION:
		s = session_find_by_id(item->session);
		if (s != NULL) {
			xasprintf(&target, "=%s:", s->name);
			cmd_find_from_session(&fs, s, 0);
		}
		break;
	case WINDOW_SWITCH_TYPE_WINDOW:
		s = session_find_by_id(item->session);
		if (s != NULL) {
			wl = winlink_find_by_index(&s->windows, item->winlink);
			if (s != NULL && wl != NULL) {
				xasprintf(&target, "=%s:%u.", s->name, wl->idx);
				cmd_find_from_winlink(&fs, wl, 0);
			}
		}
		break;
	}
	if (target != NULL) {
		mode_tree_run_command(c, &fs, data->command, target);
		free(target);
	}
#endif
}

static void
window_switch_key(struct window_mode_entry *wme, __unused struct client *c,
    __unused struct session *s, __unused struct winlink *wl, key_code key,
    __unused struct mouse_event *m)
{
	struct window_pane		*wp = wme->wp;
	struct window_switch_modedata	*data = wme->data;
	struct utf8_data		 ud, *udp;
	char				*f;
	u_int				 i;

	switch (key) {
	case '\r':
		//window_switch_select_current(data, c);
		/* FALLTHROUGH */
	case 'q':
	case '\033': /* Escape */
		window_pane_reset_mode(wp);
		return;
	case KEYC_BSPACE:
		udp = utf8_fromcstr(data->filter);
		for (i = 0; udp[i].size != 0; i++)
			;
		if (i != 0)
			i--;
		udp[i].size = 0;
		free(data->filter);
		data->filter = utf8_tocstr(udp);
		break;
	default:
		if (KEYC_IS_UNICODE(key))
			utf8_to_data(key, &ud);
		else {
			if (key <= 0x1f || key >= 0x7f)
				return;
			utf8_set(&ud, key);
		}
		xasprintf(&f, "%s%.*s", data->filter, (int)ud.size, ud.data);
		free(data->filter);
		data->filter = f;
		break;
	}

	window_switch_build(data);
	window_switch_draw_screen(wme);
	wp->flags |= PANE_REDRAW;
}
