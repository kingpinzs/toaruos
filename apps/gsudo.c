/* vim: tabstop=4 shiftwidth=4 noexpandtab
 * This file is part of ToaruOS and is released under the terms
 * of the NCSA / University of Illinois License - see LICENSE.md
 * Copyright (C) 2014-2017 K. Lange
 *
 * gsudo - graphical implementation of sudo
 *
 * probably even less secure than the original
 *
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include <toaru/auth.h>
#include <toaru/yutani.h>
#include <toaru/graphics.h>
#include <toaru/sdf.h>

#define FONT_SIZE_MAIN 20
#define FONT_SIZE_PASSWD 25

uint32_t child = 0;

int main(int argc, char ** argv) {

	int fails = 0;

	if (argc < 2) {
		return 1;
	}


	int width = 300;
	int height = 200;

	int uid;
	int error = 0;

	yutani_t * yctx = yutani_init();

	int left = (yctx->display_width - width) / 2;
	int top = (yctx->display_height - height) / 2;

	yutani_window_t * window = yutani_window_create(yctx, width, height);
	yutani_window_move(yctx, window, left, top);

	gfx_context_t * ctx = init_graphics_yutani_double_buffer(window);

	while (1) {
		char * username = getenv("USER");
		char * password = calloc(sizeof(char) * 1024,1);
		int i = 0;

		while (1) {
			draw_fill(ctx, rgba(0,0,0,200));
			int h = height-1;
			int w = width-1;
			draw_line(ctx, 0,0,0,h, rgb(255,0,0));
			draw_line(ctx, w,w,0,h, rgb(255,0,0));
			draw_line(ctx, 0,w,0,0, rgb(255,0,0));
			draw_line(ctx, 0,w,h,h, rgb(255,0,0));

			char prompt_message[512];
			sprintf(prompt_message, "Enter password for '%s'", username);
			draw_sdf_string(ctx, (width - draw_sdf_string_width(prompt_message, FONT_SIZE_MAIN, SDF_FONT_THIN)) / 2, 20, prompt_message, FONT_SIZE_MAIN, rgb(255, 255, 255), SDF_FONT_THIN);

			sprintf(prompt_message, "requested by %s", argv[1]);
			draw_sdf_string(ctx, (width - draw_sdf_string_width(prompt_message, FONT_SIZE_MAIN, SDF_FONT_THIN)) / 2, 150, prompt_message, FONT_SIZE_MAIN, rgb(255, 255, 255), SDF_FONT_THIN);

			if (error) {
				sprintf(prompt_message, "Try again. %d failures.", fails);
				draw_sdf_string(ctx, (width - draw_sdf_string_width(prompt_message, FONT_SIZE_MAIN, SDF_FONT_THIN)) / 2, 50, prompt_message, FONT_SIZE_MAIN, rgb(255, 0, 0), SDF_FONT_THIN);

			}

			char password_circles[512] = {0};;
			strcpy(password_circles, "");
			for (unsigned int i = 0; i < strlen(password) && i < 512/4; ++i) {
				strcat(password_circles, "\007");
			}
			draw_sdf_string(ctx, (width - draw_sdf_string_width(password_circles, FONT_SIZE_PASSWD, SDF_FONT_THIN)) / 2, 80, password_circles, FONT_SIZE_PASSWD, rgb(255, 255, 255), SDF_FONT_THIN);

			flip(ctx);
			yutani_flip(yctx, window);

			yutani_msg_t * msg = yutani_poll(yctx);

			switch (msg->type) {
				case YUTANI_MSG_KEY_EVENT:
					{
						struct yutani_msg_key_event * ke = (void*)msg->data;
						if (ke->event.action == KEY_ACTION_DOWN) {
							if (ke->event.keycode == KEY_ESCAPE) {
								return 1;
							}
							if (ke->event.keycode == '\n') {
								goto _done;
							} else  if (ke->event.key == 8) {
								if (i > 0) i--;
								password[i] = '\0';
							} else if (ke->event.key) {
								password[i] = ke->event.key;
								password[i+1] = '\0';
								i++;
							}
						}
					}
					break;
			}
		}

_done:
		uid = toaru_auth_check_pass(username, password);

		if (uid < 0) {
			fails++;
			if (fails == 3) {
				break;
			}
			error = 1;
			continue;
		}

		char ** args = &argv[1];
		return execvp(args[0], args);
	}

	return 1;
}
