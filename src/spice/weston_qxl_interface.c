/*
 * Copyright © 2013-2016 Yury Shvedov <shved@lvk.cs.msu.su>
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <spice/qxl_dev.h>
#include <spice.h>
#include <spice/macros.h>
#include <wayland-util.h>
#include <unistd.h>

#include "weston_spice_interfaces.h"
#include "compositor-spice.h"

//Not actually need.
QXLDevMemSlot slot = {
.slot_group_id = MEMSLOT_GROUP,
.slot_id = 0,
.generation = 0,
.virt_start = 0,
.virt_end = ~0,
.addr_delta = 0,
.qxl_ram_size = ~0,
};

#define MAX_COMMAND_NUM 1024
#define MAX_WAIT_ITERATIONS 10
#define WAIT_ITERATION_TIME 100

static struct {
    QXLCommandExt *vector [MAX_COMMAND_NUM];
    int start;
    int end;
} commands = {
    .start = 0,
    .end = 0,
};

#define ASSERT_COMMANDS assert (\
    (commands.end - commands.start <= MAX_COMMAND_NUM) && \
    (commands.end >= commands.start) )

static int
push_command (spice_backend_t *qxl, QXLCommandExt *cmd)
{
    int i = 0;
    int count;

    ASSERT_COMMANDS;

    while ( (count  = commands.end - commands.start) >= MAX_COMMAND_NUM) {
        //may be decremented from worker thread.
        if (i >= MAX_WAIT_ITERATIONS) {
            weston_log("Spice command queue overload");
            return FALSE;
        }
        ++i;
        usleep(WAIT_ITERATION_TIME);
    }
    commands.vector[commands.end % MAX_COMMAND_NUM] = cmd;
    ++commands.end;
    return TRUE;
}

static void
weston_spice_attache_worker (QXLInstance *sin, QXLWorker *qxl_worker)
{
    spice_backend_t *qxl = wl_container_of(sin, qxl, display_sin);

    if (qxl->worker != NULL) { //Only one worker per session
        if (qxl->worker == qxl_worker) {
            weston_log("Superfluous %s ignored", __func__);
        } else {
            weston_log("Superfluous %s with different worker ignored",
                        __func__);
        }
        return;
    }
    spice_qxl_add_memslot(&qxl->display_sin, &slot);

    qxl->worker = qxl_worker;
}
static void
weston_spice_set_compression_level (QXLInstance *sin, int level)
{
    spice_backend_t *qxl = wl_container_of(sin, qxl, display_sin);

    //FIXME implement
}
static void
weston_spice_set_mm_time(QXLInstance *sin, uint32_t mm_time)
{
    spice_backend_t *qxl = wl_container_of(sin, qxl, display_sin);

    qxl->mm_clock = mm_time;
}
static void
weston_spice_get_init_info(QXLInstance *sin, QXLDevInitInfo *info)
{
    spice_backend_t *qxl = wl_container_of(sin, qxl, display_sin);

    memset (info,0,sizeof(*info));

    info->num_memslots = NUM_MEMSLOTS;
    info->num_memslots_groups = NUM_MEMSLOTS_GROUPS;
    info->memslot_id_bits = MEMSLOT_ID_BITS;
    info->memslot_gen_bits = MEMSLOT_GEN_BITS;
    info->n_surfaces = NUM_SURFACES;
}
static int
weston_spice_get_command(QXLInstance *sin, struct QXLCommandExt *ext)
{
    spice_backend_t *qxl = wl_container_of(sin, qxl, display_sin);
    int count = commands.end - commands.start;

    memset (ext,0,sizeof(*ext));

    if (count > 0) {
        *ext = *commands.vector[commands.start];
        ++commands.start;
        if ( commands.start >= MAX_COMMAND_NUM ) {
            commands.start %= MAX_COMMAND_NUM;
            commands.end %= MAX_COMMAND_NUM;
        }
        ASSERT_COMMANDS;

        return TRUE;
    }

    return FALSE;
}
static int
weston_spice_req_cmd_notification(QXLInstance *sin)
{
    spice_backend_t *qxl = wl_container_of(sin, qxl, display_sin);

    /* This and req_cursor_notification needed for
     * client showing
     */
    return TRUE;
}
static void
weston_spice_release_resource(QXLInstance *sin,
                                       struct QXLReleaseInfoExt info)
{
    spice_backend_t *qxl = wl_container_of(sin, qxl, display_sin);
    struct spice_release_info *ri;

    assert (info.group_id == MEMSLOT_GROUP);
    ri = (struct spice_release_info*)(unsigned long)info.info->id;

    ri->destructor(ri);
}

/*
 * copy-paste from
 * spice/server/tests/test_display_base.b
 *
 * from here
 */

#define CURSOR_WIDTH 32
#define CURSOR_HEIGHT 32

static struct {
    QXLCursor cursor;
    uint8_t data[CURSOR_WIDTH * CURSOR_HEIGHT * 4]; // 32bit per pixel
} cursor;
static void cursor_init(void)
{
    cursor.cursor.header.unique = 0;
    cursor.cursor.header.type = SPICE_CURSOR_TYPE_COLOR32;
    cursor.cursor.header.width = CURSOR_WIDTH;
    cursor.cursor.header.height = CURSOR_HEIGHT;
    cursor.cursor.header.hot_spot_x = 0;
    cursor.cursor.header.hot_spot_y = 0;
    cursor.cursor.data_size = CURSOR_WIDTH * CURSOR_HEIGHT * 4;

    // X drivers addes it to the cursor size because it could be
    // cursor data information or another cursor related stuffs.
    // Otherwise, the code will break in client/cursor.cpp side,
    // that expect the data_size plus cursor information.
    // Blame cursor protocol for this. :-)
    cursor.cursor.data_size += 128;
    cursor.cursor.chunk.data_size = cursor.cursor.data_size;
    cursor.cursor.chunk.prev_chunk = cursor.cursor.chunk.next_chunk = 0;
}

/*
 * till here
 */

struct cursor_cmd {
    struct spice_release_info base;

    QXLCommandExt ext;
    QXLCursorCmd cursor_cmd;
};

static int
weston_spice_get_cursor_command(QXLInstance *sin, struct QXLCommandExt *ext)
{
    spice_backend_t *b = wl_container_of(sin, b, display_sin);
    static int set = TRUE;
    struct cursor_cmd *cmd;
    static wl_fixed_t x = 0, y = 0;
    struct weston_pointer *pointer;

    //Not used for now.
    return FALSE;

    /*if (!b->core_seat.has_pointer) {
        return FALSE;
    }
* looks like it depricated
    */
//    pointer = b->core_seat.pointer;

    if ( !set &&
            x == pointer->x &&
            y == pointer->y )
    {
        return FALSE;
    }

    x = pointer->x;
    y = pointer->y;

    cmd = calloc (1, sizeof *cmd);
    cmd->cursor_cmd.release_info.id = (unsigned long)cmd;
    cmd->base.destructor = release_simple;

    if (set) {
        cursor_init();
        cmd->cursor_cmd.type = QXL_CURSOR_SET;
        cmd->cursor_cmd.u.set.position.x = 0;
        cmd->cursor_cmd.u.set.position.y = 0;
        cmd->cursor_cmd.u.set.visible = TRUE;
        cmd->cursor_cmd.u.set.shape = (unsigned long)&cursor;
        // only a white rect (32x32) as cursor
        memset(cursor.data, 0xff, sizeof(cursor.data));
        set = 0;
    } else {
        cmd->cursor_cmd.type = QXL_CURSOR_MOVE;
        cmd->cursor_cmd.u.position.x = x;
        cmd->cursor_cmd.u.position.y = y;
    }

    cmd->ext.cmd.data = (unsigned long)&cmd->cursor_cmd;
    cmd->ext.cmd.type = QXL_CMD_CURSOR;
    cmd->ext.group_id = MEMSLOT_GROUP;
    cmd->ext.flags    = 0;
    *ext = cmd->ext;

    return TRUE;
}
static int
weston_spice_req_cursor_notification(QXLInstance *sin)
{
    spice_backend_t *qxl = wl_container_of(sin, qxl, display_sin);

    //FIXME implemet

    /* This and req_cmd_notification needed for
     * client showing
     */
    return TRUE;
}
static void
weston_spice_notify_update(QXLInstance *sin, uint32_t update_id)
{
    spice_backend_t *qxl = wl_container_of(sin, qxl, display_sin);

    //FIXME implemet

}
static int
weston_spice_flush_resources(QXLInstance *sin)
{
    spice_backend_t *qxl = wl_container_of(sin, qxl, display_sin);

    //FIXME implemet

    return 0;
}
static QXLInterface weston_qxl_interface = {
    .base.type                  = SPICE_INTERFACE_QXL,
    .base.description           = "weston qxl gpu",
    .base.major_version         = SPICE_INTERFACE_QXL_MAJOR,
    .base.minor_version         = SPICE_INTERFACE_QXL_MINOR,

    .attache_worker             = weston_spice_attache_worker,
    .set_compression_level      = weston_spice_set_compression_level,
    .set_mm_time                = weston_spice_set_mm_time,
    .get_init_info              = weston_spice_get_init_info,

    .get_command                = weston_spice_get_command,
    .req_cmd_notification       = weston_spice_req_cmd_notification,
    .release_resource           = weston_spice_release_resource,
    .get_cursor_command         = weston_spice_get_cursor_command,
    .req_cursor_notification    = weston_spice_req_cursor_notification,
    .notify_update              = weston_spice_notify_update,
    .flush_resources            = weston_spice_flush_resources,
};

void
weston_spice_qxl_init (spice_backend_t *qxl)
{
    static int qxl_count = 0;

    if ( ++qxl_count > 1 ) {
        weston_log("Only one instance of qxl interface supported");
        exit(1);
    }

    qxl->display_sin.base.sif = &weston_qxl_interface.base;
    qxl->display_sin.id = 0;
    qxl->display_sin.st = (struct QXLState*)qxl;
    qxl->push_command = push_command;
}
void
weston_spice_qxl_destroy (spice_backend_t *b)
{
    struct spice_release_info *ri;
    QXLReleaseInfo *info;
    while (commands.start < commands.end) {
        ++commands.start;
        //Ri is on the top of all QXL commands
        info = (QXLReleaseInfo *)
            commands.vector[commands.start - 1]->cmd.data;
        ri = (struct spice_release_info*)(unsigned long) info->id;
            ri->destructor(ri);
    }
}
