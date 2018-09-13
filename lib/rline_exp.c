/* vim: tabstop=4 shiftwidth=4 noexpandtab
 * This file is part of ToaruOS and is released under the terms
 * of the NCSA / University of Illinois License - see LICENSE.md
 * Copyright (C) 2018 K. Lange
 *
 * EXPERIMENTAL rline replacement with syntax highlight, based on
 * bim's lightlighting and line editing.
 *
 * Still needs tab completion, history, etc. integration, and a
 * LOT of code cleanup because this is all basically just cut
 * and pasted directly out of bim.
 *
 * Some key bindings should also be added (some of which are missing
 * in bim as well) like ^W.
 */
#define _XOPEN_SOURCE
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include <termios.h>
#include <string.h>
#include <wchar.h>
#include <unistd.h>
#include <locale.h>
#include <sys/ioctl.h>

#include <toaru/rline.h>

#define ENTER_KEY     '\n'
#define BACKSPACE_KEY 0x08
#define DELETE_KEY    0x7F

typedef struct {
	uint32_t display_width:4;
	uint32_t flags:7;
	uint32_t codepoint:21;
} __attribute__((packed)) char_t;

typedef struct {
	int available;
	int actual;
	int istate;
	char_t   text[0];
} line_t;

line_t * the_line = NULL;

static int loading = 0;
static int column = 0;
static int offset = 0;
static int width =  0;
static int buf_size_max = 0;

/**
 * TODO: Need to make prompt configurable.
 *       Ideally, sh, etc. should generate
 *       a prompt string from $PS1 and also
 *       calculate it's width; we need \[\]
 *       for that to work correctly.
 */
static int prompt_width = 2;
static char * prompt = "> ";
static int prompt_right_width = 4;
static char * prompt_right = " :) ";

static char ** shell_commands = {0};
static int shell_commands_len = 0;

int rline_exp_set_shell_commands(char ** cmds, int len) {
	shell_commands = cmds;
	shell_commands_len = len;
	return 0;
}

int rline_exp_set_prompts(char * left, char * right, int left_width, int right_width) {
	prompt = left;
	prompt_right = right;
	prompt_width = left_width;
	prompt_right_width = right_width;
	return 0;
}

static rline_callback_t tab_complete_func = NULL;

int rline_exp_set_tab_complete_func(rline_callback_t func) {
	tab_complete_func = func;
	return 0;
}

static int to_eight(uint32_t codepoint, char * out) {
	memset(out, 0x00, 7);

	if (codepoint < 0x0080) {
		out[0] = (char)codepoint;
	} else if (codepoint < 0x0800) {
		out[0] = 0xC0 | (codepoint >> 6);
		out[1] = 0x80 | (codepoint & 0x3F);
	} else if (codepoint < 0x10000) {
		out[0] = 0xE0 | (codepoint >> 12);
		out[1] = 0x80 | ((codepoint >> 6) & 0x3F);
		out[2] = 0x80 | (codepoint & 0x3F);
	} else if (codepoint < 0x200000) {
		out[0] = 0xF0 | (codepoint >> 18);
		out[1] = 0x80 | ((codepoint >> 12) & 0x3F);
		out[2] = 0x80 | ((codepoint >> 6) & 0x3F);
		out[3] = 0x80 | ((codepoint) & 0x3F);
	} else if (codepoint < 0x4000000) {
		out[0] = 0xF8 | (codepoint >> 24);
		out[1] = 0x80 | (codepoint >> 18);
		out[2] = 0x80 | ((codepoint >> 12) & 0x3F);
		out[3] = 0x80 | ((codepoint >> 6) & 0x3F);
		out[4] = 0x80 | ((codepoint) & 0x3F);
	} else {
		out[0] = 0xF8 | (codepoint >> 30);
		out[1] = 0x80 | ((codepoint >> 24) & 0x3F);
		out[2] = 0x80 | ((codepoint >> 18) & 0x3F);
		out[3] = 0x80 | ((codepoint >> 12) & 0x3F);
		out[4] = 0x80 | ((codepoint >> 6) & 0x3F);
		out[5] = 0x80 | ((codepoint) & 0x3F);
	}

	return strlen(out);
}

#define UTF8_ACCEPT 0
#define UTF8_REJECT 1

static inline uint32_t decode(uint32_t* state, uint32_t* codep, uint32_t byte) {
	static int state_table[32] = {
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0xxxxxxx */
		1,1,1,1,1,1,1,1,                 /* 10xxxxxx */
		2,2,2,2,                         /* 110xxxxx */
		3,3,                             /* 1110xxxx */
		4,                               /* 11110xxx */
		1                                /* 11111xxx */
	};

	static int mask_bytes[32] = {
		0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,
		0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,0x7F,
		0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
		0x1F,0x1F,0x1F,0x1F,
		0x0F,0x0F,
		0x07,
		0x00
	};

	static int next[5] = {
		0,
		1,
		0,
		2,
		3
	};

	if (*state == UTF8_ACCEPT) {
		*codep = byte & mask_bytes[byte >> 3];
		*state = state_table[byte >> 3];
	} else if (*state > 0) {
		*codep = (byte & 0x3F) | (*codep << 6);
		*state = next[*state];
	}
	return *state;
}

static int codepoint_width(wchar_t codepoint) {
	if (codepoint == '\t') {
		return 1; /* Recalculate later */
	}
	if (codepoint < 32) {
		/* We render these as ^@ */
		return 2;
	}
	if (codepoint == 0x7F) {
		/* Renders as ^? */
		return 2;
	}
	if (codepoint > 0x7f && codepoint < 0xa0) {
		/* Upper control bytes <xx> */
		return 4;
	}
	if (codepoint == 0xa0) {
		/* Non-breaking space _ */
		return 1;
	}
	/* Skip wcwidth for anything under 256 */
	if (codepoint > 256) {
		/* Higher codepoints may be wider (eg. Japanese) */
		int out = wcwidth(codepoint);
		if (out >= 1) return out;
		/* Invalid character, render as [U+ABCD] or [U+ABCDEF] */
		return (codepoint < 0x10000) ? 8 : 10;
	}
	return 1;
}

static const char * COLOR_FG        = "@9";
static const char * COLOR_BG        = "@9";
static const char * COLOR_ALT_FG    = "@5";
static const char * COLOR_ALT_BG    = "@9";
static const char * COLOR_NUMBER_FG = "@3";
static const char * COLOR_NUMBER_BG = "@9";
static const char * COLOR_STATUS_FG = "@7";
static const char * COLOR_STATUS_BG = "@4";
static const char * COLOR_TABBAR_BG = "@4";
static const char * COLOR_TAB_BG    = "@4";
static const char * COLOR_KEYWORD   = "@4";
static const char * COLOR_STRING    = "@2";
static const char * COLOR_COMMENT   = "@5";
static const char * COLOR_TYPE      = "@3";
static const char * COLOR_PRAGMA    = "@1";
static const char * COLOR_NUMERAL   = "@1";
static const char * COLOR_ERROR_FG  = "@7";
static const char * COLOR_ERROR_BG  = "@1";
static const char * COLOR_SEARCH_FG = "@0";
static const char * COLOR_SEARCH_BG = "@3";
static const char * COLOR_SELECTBG  = "@7";
static const char * COLOR_SELECTFG  = "@0";
static const char * COLOR_RED       = "@1";
static const char * COLOR_GREEN     = "@2";

void rline_exp_load_colorscheme_default(void) {
	COLOR_FG        = "@9";
	COLOR_BG        = "@9";
	COLOR_ALT_FG    = "@5";
	COLOR_ALT_BG    = "@9";
	COLOR_NUMBER_FG = "@3";
	COLOR_NUMBER_BG = "@9";
	COLOR_STATUS_FG = "@7";
	COLOR_STATUS_BG = "@4";
	COLOR_TABBAR_BG = "@4";
	COLOR_TAB_BG    = "@4";
	COLOR_KEYWORD   = "@4";
	COLOR_STRING    = "@2";
	COLOR_COMMENT   = "@5";
	COLOR_TYPE      = "@3";
	COLOR_PRAGMA    = "@1";
	COLOR_NUMERAL   = "@1";
	COLOR_ERROR_FG  = "@7";
	COLOR_ERROR_BG  = "@1";
	COLOR_SEARCH_FG = "@0";
	COLOR_SEARCH_BG = "@3";
	COLOR_SELECTBG  = "@7";
	COLOR_SELECTFG  = "@0";
	COLOR_RED       = "@1";
	COLOR_GREEN     = "@2";
}

void rline_exp_load_colorscheme_sunsmoke(void) {
	COLOR_FG        = "2;230;230;230";
	COLOR_BG        = "@9";
	COLOR_ALT_FG    = "2;122;122;122";
	COLOR_ALT_BG    = "2;46;43;46";
	COLOR_NUMBER_FG = "2;150;139;57";
	COLOR_NUMBER_BG = "2;0;0;0";
	COLOR_STATUS_FG = "2;230;230;230";
	COLOR_STATUS_BG = "2;71;64;58";
	COLOR_TABBAR_BG = "2;71;64;58";
	COLOR_TAB_BG    = "2;71;64;58";
	COLOR_KEYWORD   = "2;51;162;230";
	COLOR_STRING    = "2;72;176;72";
	COLOR_COMMENT   = "2;158;153;129;3";
	COLOR_TYPE      = "2;230;206;110";
	COLOR_PRAGMA    = "2;194;70;54";
	COLOR_NUMERAL   = "2;230;43;127";

	COLOR_ERROR_FG  = "5;15";
	COLOR_ERROR_BG  = "5;196";
	COLOR_SEARCH_FG = "5;234";
	COLOR_SEARCH_BG = "5;226";

	COLOR_SELECTFG  = "2;0;43;54";
	COLOR_SELECTBG  = "2;147;161;161";

	COLOR_RED       = "2;222;53;53";
	COLOR_GREEN     = "2;55;167;0";
}
/**
 * Syntax highlighting flags.
 */
#define FLAG_NONE      0
#define FLAG_KEYWORD   1
#define FLAG_STRING    2
#define FLAG_COMMENT   3
#define FLAG_TYPE      4
#define FLAG_PRAGMA    5
#define FLAG_NUMERAL   6
#define FLAG_SELECT    7
#define FLAG_STRING2   8
#define FLAG_DIFFPLUS  9
#define FLAG_DIFFMINUS 10

#define FLAG_CONTINUES (1 << 6)

/**
 * Syntax definition for ToaruOS shell
 */
static char * syn_sh_keywords[] = {
	"cd","exit","export","help","history","if",
	"empty?","equals?","return","export-cmd",
	"source","exec","not","while","then","else",
	NULL,
};

static int variable_char(uint8_t c) {
	if (c >= 'A' && c <= 'Z')  return 1;
	if (c >= 'a' && c <= 'z') return 1;
	if (c >= '0' && c <= '9')  return 1;
	if (c == '_') return 1;
	if (c == '?') return 1;
	return 0;
}

static int syn_sh_extended(line_t * line, int i, int c, int last, int * out_left) {
	(void)last;

	if (c == '#') {
		*out_left = (line->actual + 1) - i;
		return FLAG_COMMENT;
	}

	if (line->text[i].codepoint == '\'') {
		int last = 0;
		for (int j = i+1; j < line->actual + 1; ++j) {
			int c = line->text[j].codepoint;
			if (last != '\\' && c == '\'') {
				*out_left = j - i;
				return FLAG_STRING;
			}
			if (last == '\\' && c == '\\') {
				last = 0;
			}
			last = c;
		}
		*out_left = (line->actual + 1) - i; /* unterminated string */
		return FLAG_STRING;
	}

	if (line->text[i].codepoint == '$' && last != '\\') {
		if (i < line->actual - 1 && line->text[i+1].codepoint == '{') {
			int j = i + 2;
			for (; j < line->actual+1; ++j) {
				if (line->text[j].codepoint == '}') break;
			}
			*out_left = (j - i);
			return FLAG_NUMERAL;
		}
		int j = i + 1;
		for (; j < line->actual + 1; ++j) {
			if (!variable_char(line->text[j].codepoint)) break;
		}
		*out_left = (j - i) - 1;
		return FLAG_NUMERAL;
	}

	if (line->text[i].codepoint == '"') {
		int last = 0;
		for (int j = i+1; j < line->actual + 1; ++j) {
			int c = line->text[j].codepoint;
			if (last != '\\' && c == '"') {
				*out_left = j - i;
				return FLAG_STRING;
			}
			if (last == '\\' && c == '\\') {
				last = 0;
			}
			last = c;
		}
		*out_left = (line->actual + 1) - i; /* unterminated string */
		return FLAG_STRING;
	}

	return 0;
}

static int syn_sh_iskeywordchar(int c) {
	if (isalnum(c)) return 1;
	if (c == '-') return 1;
	if (c == '_') return 1;
	if (c == '?') return 1;
	return 0;
}

/**
 * Convert syntax hilighting flag to color code
 */
static const char * flag_to_color(int _flag) {
	int flag = _flag & 0x3F;
	switch (flag) {
		case FLAG_KEYWORD:
			return COLOR_KEYWORD;
		case FLAG_STRING:
		case FLAG_STRING2: /* allows python to differentiate " and ' */
			return COLOR_STRING;
		case FLAG_COMMENT:
			return COLOR_COMMENT;
		case FLAG_TYPE:
			return COLOR_TYPE;
		case FLAG_NUMERAL:
			return COLOR_NUMERAL;
		case FLAG_PRAGMA:
			return COLOR_PRAGMA;
		case FLAG_DIFFPLUS:
			return COLOR_GREEN;
		case FLAG_DIFFMINUS:
			return COLOR_RED;
		case FLAG_SELECT:
			return COLOR_FG;
		default:
			return COLOR_FG;
	}
}


static void set_colors(const char * fg, const char * bg) {
	printf("\033[22;23;");
	if (*bg == '@') {
		int _bg = atoi(bg+1);
		if (_bg < 10) {
			printf("4%d;", _bg);
		} else {
			printf("10%d;", _bg-10);
		}
	} else {
		printf("48;%s;", bg);
	}
	if (*fg == '@') {
		int _fg = atoi(fg+1);
		if (_fg < 10) {
			printf("3%dm", _fg);
		} else {
			printf("9%dm", _fg-10);
		}
	} else {
		printf("38;%sm", fg);
	}
	fflush(stdout);
}

/**
 * Set just the foreground color
 *
 * (See set_colors above)
 */
static void set_fg_color(const char * fg) {
	printf("\033[22;23;");
	if (*fg == '@') {
		int _fg = atoi(fg+1);
		if (_fg < 10) {
			printf("3%dm", _fg);
		} else {
			printf("9%dm", _fg-10);
		}
	} else {
		printf("38;%sm", fg);
	}
	fflush(stdout);
}

static void draw_prompt(void) {
	printf("\033[0m\r%s", prompt);
}

static void render_line(void) {
	printf("\033[?25l");
	draw_prompt();

	int i = 0; /* Offset in char_t line data entries */
	int j = 0; /* Offset in terminal cells */

	const char * last_color = NULL;

	/* Set default text colors */
	set_colors(COLOR_FG, COLOR_BG);

	/*
	 * When we are rendering in the middle of a wide character,
	 * we render -'s to fill the remaining amount of the 
	 * charater's width
	 */
	int remainder = 0;

	line_t * line = the_line;

	/* For each character in the line ... */
	while (i < line->actual) {

		/* If there is remaining text... */
		if (remainder) {

			/* If we should be drawing by now... */
			if (j >= offset) {
				/* Fill remainder with -'s */
				set_colors(COLOR_ALT_FG, COLOR_ALT_BG);
				printf("-");
				set_colors(COLOR_FG, COLOR_BG);
			}

			/* One less remaining width cell to fill */
			remainder--;

			/* Terminal offset moves forward */
			j++;

			/*
			 * If this was the last remaining character, move to
			 * the next codepoint in the line
			 */
			if (remainder == 0) {
				i++;
			}

			continue;
		}

		/* Get the next character to draw */
		char_t c = line->text[i];

		/* If we should be drawing by now... */
		if (j >= offset) {

			/* If this character is going to fall off the edge of the screen... */
			if (j - offset + c.display_width >= width - prompt_width) {
				/* We draw this with special colors so it isn't ambiguous */
				set_colors(COLOR_ALT_FG, COLOR_ALT_BG);

				/* If it's wide, draw ---> as needed */
				while (j - offset < width - prompt_width - 1) {
					printf("-");
					j++;
				}

				/* End the line with a > to show it overflows */
				printf(">");
				set_colors(COLOR_FG, COLOR_BG);
				return;
			}

			/* Syntax hilighting */
			const char * color = flag_to_color(c.flags);
			if (!last_color || strcmp(color, last_color)) {
				set_fg_color(color);
				last_color = color;
			}

			/* Render special characters */
			if (c.codepoint == '\t') {
				set_colors(COLOR_ALT_FG, COLOR_ALT_BG);
				printf("»");
				for (int i = 1; i < c.display_width; ++i) {
					printf("·");
				}
				set_colors(last_color ? last_color : COLOR_FG, COLOR_BG);
			} else if (c.codepoint < 32) {
				/* Codepoints under 32 to get converted to ^@ escapes */
				set_colors(COLOR_ALT_FG, COLOR_ALT_BG);
				printf("^%c", '@' + c.codepoint);
				set_colors(last_color ? last_color : COLOR_FG, COLOR_BG);
			} else if (c.codepoint == 0x7f) {
				set_colors(COLOR_ALT_FG, COLOR_ALT_BG);
				printf("^?");
				set_colors(last_color ? last_color : COLOR_FG, COLOR_BG);
			} else if (c.codepoint > 0x7f && c.codepoint < 0xa0) {
				set_colors(COLOR_ALT_FG, COLOR_ALT_BG);
				printf("<%2x>", c.codepoint);
				set_colors(last_color ? last_color : COLOR_FG, COLOR_BG);
			} else if (c.codepoint == 0xa0) {
				set_colors(COLOR_ALT_FG, COLOR_ALT_BG);
				printf("_");
				set_colors(last_color ? last_color : COLOR_FG, COLOR_BG);
			} else if (c.display_width == 8) {
				set_colors(COLOR_ALT_FG, COLOR_ALT_BG);
				printf("[U+%04x]", c.codepoint);
				set_colors(last_color ? last_color : COLOR_FG, COLOR_BG);
			} else if (c.display_width == 10) {
				set_colors(COLOR_ALT_FG, COLOR_ALT_BG);
				printf("[U+%06x]", c.codepoint);
				set_colors(last_color ? last_color : COLOR_FG, COLOR_BG);
			} else if (c.codepoint == ' ' && i == line->actual - 1) {
				/* Special case: space at end of line */
				set_colors(COLOR_ALT_FG, COLOR_ALT_BG);
				printf("·");
				set_colors(COLOR_FG, COLOR_BG);
			} else {
				/* Normal characters get output */
				char tmp[7]; /* Max six bytes, use 7 to ensure last is always nil */
				to_eight(c.codepoint, tmp);
				printf("%s", tmp);
			}

			/* Advance the terminal cell offset by the render width of this character */
			j += c.display_width;

			/* Advance to the next character */
			i++;
		} else if (c.display_width > 1) {
			/*
			 * If this is a wide character but we aren't ready to render yet,
			 * we may need to draw some filler text for the remainder of its
			 * width to ensure we don't jump around when horizontally scrolling
			 * past wide characters.
			 */
			remainder = c.display_width - 1;
			j++;
		} else {
			/* Regular character, not ready to draw, advance without doing anything */
			j++;
			i++;
		}
	}
	for (; j < width + offset - prompt_width; ++j) {
		printf(" ");
	}
	printf("\033[0m%s", prompt_right);
}

static int check_line(line_t * line, int c, char * str, int last) {
	if (syn_sh_iskeywordchar(last)) return 0;
	for (int i = c; i < line->actual; ++i, ++str) {
		if (*str == '\0' && !syn_sh_iskeywordchar(line->text[i].codepoint)) return 1;
		if (line->text[i].codepoint == *str) continue;
		return 0;
	}
	if (*str == '\0') return 1;
	return 0;
}

static void recalculate_syntax(line_t * line) {

	/* Start from the line's stored in initial state */
	int state = line->istate;
	int left  = 0;
	int last  = 0;

	for (int i = 0; i < line->actual; last = line->text[i++].codepoint) {
		if (!left) state = 0;

		if (state) {
			/* Currently hilighting, have `left` characters remaining with this state */
			left--;
			line->text[i].flags = state;

			if (!left) {
				/* Done hilighting this state, go back to parsing on next character */
				state = 0;
			}

			/* If we are hilighting something, don't parse */
			continue;
		}

		int c = line->text[i].codepoint;
		line->text[i].flags = FLAG_NONE;

		/* Language-specific syntax hilighting */
		int s = syn_sh_extended(line,i,c,last,&left);
		if (s) {
			state = s;
			goto _continue;
		}

		/* Keywords */
		for (char ** kw = syn_sh_keywords; *kw; kw++) {
			int c = check_line(line, i, *kw, last);
			if (c == 1) {
				left = strlen(*kw)-1;
				state = FLAG_KEYWORD;
				goto _continue;
			}
		}

		for (int s = 0; s < shell_commands_len; ++s) {
			int c = check_line(line, i, shell_commands[s], last);
			if (c == 1) {
				left = strlen(shell_commands[s])-1;
				state = FLAG_KEYWORD;
				goto _continue;
			}
		}

_continue:
		line->text[i].flags = state;
	}

	state = 0;
}

static line_t * line_create(void) {
	line_t * line = malloc(sizeof(line_t) + sizeof(char_t) * 32);
	line->available = 32;
	line->actual    = 0;
	line->istate    = 0;
	return line;
}

static line_t * line_insert(line_t * line, char_t c, int offset) {

	/* If there is not enough space... */
	if (line->actual == line->available) {
		/* Expand the line buffer */
		line->available *= 2;
		line = realloc(line, sizeof(line_t) + sizeof(char_t) * line->available);
	}

	/* If this was not the last character, then shift remaining characters forward. */
	if (offset < line->actual) {
		memmove(&line->text[offset+1], &line->text[offset], sizeof(char_t) * (line->actual - offset));
	}

	/* Insert the new character */
	line->text[offset] = c;

	/* There is one new character in the line */
	line->actual += 1;

	if (!loading) {
		recalculate_syntax(line);
	}

	return line;
}

static void get_size(void) {
	struct winsize w;
	ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
	width = w.ws_col - prompt_right_width;
}

static void place_cursor_actual(void) {
	get_size();

	int x = prompt_width + 1 - offset;
	for (int i = 0; i < column; ++i) {
		char_t * c = &the_line->text[i];
		x += c->display_width;
	}

	if (x > width - 1) {
		/* Adjust the offset appropriately to scroll horizontally */
		int diff = x - (width - 1);
		offset += diff;
		x -= diff;
		render_line();
	}

	/* Same for scrolling horizontally to the left */
	if (x < prompt_width + 1) {
		int diff = (prompt_width + 1) - x;
		offset -= diff;
		x += diff;
		render_line();
	}

	printf("\033[?25h\033[%dG", x);
	fflush(stdout);
}

static void line_delete(line_t * line, int offset) {

	/* Can't delete character before start of line. */
	if (offset == 0) return;

	/* If this isn't the last character, we need to move all subsequent characters backwards */
	if (offset < line->actual) {
		memmove(&line->text[offset-1], &line->text[offset], sizeof(char_t) * (line->actual - offset));
	}

	/* The line is one character shorter */
	line->actual -= 1;

	if (!loading) {
		recalculate_syntax(line);
	}
}

static void delete_at_cursor(void) {
	if (column > 0) {
		line_delete(the_line, column);
		column--;
		if (offset > 0) offset--;
		render_line();
		place_cursor_actual();
	}
}

static void delete_word(void) {
	if (!the_line->actual) return;
	if (!column) return;

	do {
		if (column > 0) {
			line_delete(the_line, column);
			column--;
			if (offset > 0) offset--;
		}
	} while (column && the_line->text[column-1].codepoint != ' ');

	render_line();
	place_cursor_actual();
}

static void insert_char(uint32_t c) {
	char_t _c;
	_c.codepoint = c;
	_c.flags = 0;
	_c.display_width = codepoint_width(c);

	the_line = line_insert(the_line, _c, column);

	column++;
	if (!loading) {
		render_line();
		place_cursor_actual();
	}
}

static void cursor_left(void) {
	if (column > 0) column--;
	place_cursor_actual();
}

static void cursor_right(void) {
	if (column < the_line->actual) column++;
	place_cursor_actual();
}

static void word_left(void) {
	if (column == 0) return;
	column--;
	while (column && the_line->text[column].codepoint == ' ') {
		column--;
	}
	while (column > 0) {
		if (the_line->text[column-1].codepoint == ' ') break;
		column--;
	}
	place_cursor_actual();
}

static void word_right(void) {
	/* TODO */
	while (column < the_line->actual && the_line->text[column].codepoint == ' ') {
		column++;
	}
	while (column < the_line->actual) {
		column++;
		if (the_line->text[column].codepoint == ' ') break;
	}
	place_cursor_actual();
}

static void cursor_home(void) {
	column = 0;
	place_cursor_actual();
}

static void cursor_end(void) {
	column = the_line->actual;
	place_cursor_actual();
}

static char temp_buffer[1024];

static void history_previous(void) {
	if (rline_scroll == 0) {
		/* Convert to temporaary buffer */
		unsigned int off = 0;
		memset(temp_buffer, 0, sizeof(temp_buffer));
		for (int j = 0; j < the_line->actual; j++) {
			char_t c = the_line->text[j];
			off += to_eight(c.codepoint, &temp_buffer[off]);
		}
	}

	if (rline_scroll < rline_history_count) {
		rline_scroll++;

		/* Copy in from history */
		the_line->actual = 0;
		column = 0;
		loading = 1;
		char * buf = rline_history_prev(rline_scroll);
		uint32_t istate = 0, c = 0;
		for (unsigned int i = 0; i < strlen(buf); ++i) {
			if (!decode(&istate, &c, buf[i])) {
				insert_char(c);
			}
		}
		loading = 0;
	}
	/* Set cursor at end */
	column = the_line->actual;
	recalculate_syntax(the_line);
	render_line();
	place_cursor_actual();
}

static void history_next(void) {
	if (rline_scroll > 1) {
		rline_scroll--;

		/* Copy in from history */
		the_line->actual = 0;
		column = 0;
		loading = 1;
		char * buf = rline_history_prev(rline_scroll);
		uint32_t istate = 0, c = 0;
		for (unsigned int i = 0; i < strlen(buf); ++i) {
			if (!decode(&istate, &c, buf[i])) {
				insert_char(c);
			}
		}
		loading = 0;
	} else if (rline_scroll == 1) {
		/* Copy in from temp */
		rline_scroll = 0;

		the_line->actual = 0;
		column = 0;
		loading = 1;
		char * buf = temp_buffer;
		uint32_t istate = 0, c = 0;
		for (unsigned int i = 0; i < strlen(buf); ++i) {
			if (!decode(&istate, &c, buf[i])) {
				insert_char(c);
			}
		}
		loading = 0;
	}
	/* Set cursor at end */
	column = the_line->actual;
	recalculate_syntax(the_line);
	render_line();
	place_cursor_actual();
}


static int handle_escape(int * this_buf, int * timeout, int c) {
	if (*timeout >=  1 && this_buf[*timeout-1] == '\033' && c == '\033') {
		this_buf[*timeout] = c;
		(*timeout)++;
		return 1;
	}
	if (*timeout >= 1 && this_buf[*timeout-1] == '\033' && c != '[') {
		*timeout = 0;
		ungetc(c, stdin);
		return 1;
	}
	if (*timeout >= 1 && this_buf[*timeout-1] == '\033' && c == '[') {
		*timeout = 1;
		this_buf[*timeout] = c;
		(*timeout)++;
		return 0;
	}
	if (*timeout >= 2 && this_buf[0] == '\033' && this_buf[1] == '[' &&
			(isdigit(c) || c == ';')) {
		this_buf[*timeout] = c;
		(*timeout)++;
		return 0;
	}
	if (*timeout >= 2 && this_buf[0] == '\033' && this_buf[1] == '[') {
		switch (c) {
			case 'A': // up
				history_previous();
				break;
			case 'B': // down
				history_next();
				break;
			case 'C': // right
				if (this_buf[*timeout-1] == '5') {
					word_right();
				} else {
					cursor_right();
				}
				break;
			case 'D': // left
				if (this_buf[*timeout-1] == '5') {
					word_left();
				} else {
					cursor_left();
				}
				break;
			case 'H': // home
				cursor_home();
				break;
			case 'F': // end
				cursor_end();
				break;
			case '~':
				switch (this_buf[*timeout-1]) {
					case '1':
						cursor_home();
						break;
					case '3':
#if 0
						if (env->mode == MODE_INSERT || env->mode == MODE_REPLACE) {
							if (env->col_no < env->lines[env->line_no - 1]->actual + 1) {
								line_delete(env->lines[env->line_no - 1], env->col_no, env->line_no - 1);
								redraw_line(env->line_no - env->offset - 1, env->line_no-1);
								set_modified();
								redraw_statusbar();
								place_cursor_actual();
							} else if (env->line_no < env->line_count) {
								merge_lines(env->lines, env->line_no);
								redraw_text();
								set_modified();
								redraw_statusbar();
								place_cursor_actual();
							}
						}
#endif
						/* Delete forward */
						break;
					case '4':
						cursor_end();
						break;
				}
				break;
			default:
				break;
		}
		*timeout = 0;
		return 0;
	}

	*timeout = 0;
	return 0;
}

struct termios old;
static void get_initial_termios(void) {
	tcgetattr(STDOUT_FILENO, &old);
}

static void set_unbuffered(void) {
	struct termios new = old;
	new.c_lflag &= (~ICANON & ~ECHO);
	tcsetattr(STDOUT_FILENO, TCSAFLUSH, &new);
}

static void set_buffered(void) {
	tcsetattr(STDOUT_FILENO, TCSAFLUSH, &old);
}

static int tabbed;

static void dummy_redraw(rline_context_t * context) {
	/* Do nothing */
}

static void call_rline_func(rline_callback_t func, rline_context_t * context) {
	uint32_t istate = 0;
	uint32_t c;
	context->quiet = 1;
	context->buffer = malloc(buf_size_max); /* TODO */
	memset(context->buffer,0,buf_size_max);
	unsigned int off = 0;
	for (int j = 0; j < the_line->actual; j++) {
		if (j == column) {
			context->offset = off;
		}
		char_t c = the_line->text[j];
		off += to_eight(c.codepoint, &context->buffer[off]);
	}
	if (column == the_line->actual) context->offset = off;
	context->tabbed = tabbed;
	rline_callbacks_t tmp = {0};
	tmp.redraw_prompt = dummy_redraw;
	context->callbacks = &tmp;
	context->collected = off;
	context->buffer[off] = '\0';
	context->requested = 1024;
	printf("\033[0m");
	func(context);
	/* Now convert back */
	loading = 1;
	int final_column = 0;
	the_line->actual = 0;
	column = 0;
	istate = 0;
	for (int i = 0; i < context->collected; ++i) {
		if (i == context->offset) {
			final_column = column;
		}
		if (!decode(&istate, &c, context->buffer[i])) {
			insert_char(c);
		}
	}
	if (context->offset == context->collected) {
		column = the_line->actual;
	} else {
		column = final_column;
	}

	tabbed = context->tabbed;

	loading = 0;
	recalculate_syntax(the_line);
	render_line();
	place_cursor_actual();

}

static int read_line(void) {
	int cin;
	uint32_t c;
	int timeout = 0;
	int this_buf[20];
	uint32_t istate = 0;

	render_line();
	place_cursor_actual();

	while ((cin = getc(stdin))) {
		if (!decode(&istate, &c, cin)) {
			if (timeout == 0) {
				if (c != '\t') tabbed = 0;
				switch (c) {
					case '\033':
						if (timeout == 0) {
							this_buf[timeout] = c;
							timeout++;
						}
						break;
					case 3:
						the_line->actual = 0;
						set_colors(COLOR_ALT_FG, COLOR_ALT_BG);
						printf("^C");
						printf("\033[0m");
						return 1;
					case 4:
						if (column == 0 && the_line->actual == 0) {
							for (char *_c = "exit"; *_c; ++_c) {
								insert_char(*_c);
							}
							return 1;
						} else {
							if (column < the_line->actual) {
								line_delete(the_line, column+1);
								if (offset > 0) offset--;
								render_line();
								place_cursor_actual();
							}
						}
						break;
					case DELETE_KEY:
					case BACKSPACE_KEY:
						delete_at_cursor();
						break;
					case ENTER_KEY:
						/* Print buffer */
						return 1;
					case 23:
						delete_word();
						break;
					case 12: /* ^L - Repaint the whole screen */
						printf("\033[2J\033[H");
						render_line();
						place_cursor_actual();
						break;
					case '\t':
						/* Tab complet e*/
						if (tab_complete_func) {
							rline_context_t context = {0};
							call_rline_func(tab_complete_func, &context);
						}
						break;
					case 18:
						{
							rline_context_t context = {0};
							call_rline_func(rline_reverse_search, &context);
							if (!context.cancel) {
								return 1;
							}
						}
						break;
					default:
						insert_char(c);
						break;
				}
			} else {
				if (handle_escape(this_buf,&timeout,c)) {
					continue;
				}
			}
		} else if (istate == UTF8_REJECT) {
			istate = 0;
		}
	}
	return 0;
}

int rline_experimental(char * buffer, int buf_size) {
	get_initial_termios();
	set_unbuffered();
	get_size();

	column = 0;
	offset = 0;
	buf_size_max = buf_size;

	char * theme = getenv("RLINE_THEME");
	if (theme && !strcmp(theme,"sunsmoke")) { /* TODO bring back theme tables */
		rline_exp_load_colorscheme_sunsmoke();
	} else {
		rline_exp_load_colorscheme_default();
	}

	the_line = line_create();
	read_line();
	printf("\033[0m\n");

	unsigned int off = 0;
	for (int j = 0; j < the_line->actual; j++) {
		char_t c = the_line->text[j];
		off += to_eight(c.codepoint, &buffer[off]);
	}

	free(the_line);

	set_buffered();

	return strlen(buffer);
}

#if 0
int main(int argc, char * argv[]) {
	char buf[1024];
	int r = rline_experimental(buf, 1024);
	fwrite(buf, 1, r, stdout);
	return 0;
}
#endif
