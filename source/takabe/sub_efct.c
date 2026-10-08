#include "sub_efct.h"

#include <sys/types.h>
#include <libgte.h>
#include <libgpu.h>

#include "common.h"
#include "libgv/libgv.h"
#include "libdg/libdg.h"
extern SVECTOR DG_Ambient;
#include "game/game.h"

typedef struct _Work
{
    GV_ACT   actor;
    OBJECT  *parent;
    SVECTOR *rotation;
    MATRIX   light[2];
    u_long   flag;
    MATRIX  *lightp;
    char     pad[0x4];
} Work;

#define EXEC_LEVEL GV_ACTOR_DAEMON

#ifndef __psyz
#define gte_pop_color(r0) __asm__ volatile ("mfc2   %0, $9;" : "=r"(r0))
#endif /* __psyz: psyz provides a portable form */


void SubEfct_800CC798(DG_MDL *model)
{
    int      base;
    u_char  *out;
    SVECTOR *vertices;
    int      n_vertices;
    int      color;

    base = 128;
    out = (u_char *)SCRPAD_ADDR;

    vertices = model->vertices;
    n_vertices = model->n_verts;

    gte_ldv3c(vertices);
    gte_nop();
    gte_rtv0_b();

    vertices += 3;

    for (; n_vertices > 0; n_vertices -= 3)
    {
        gte_pop_color(color);
        color >>= 3;
        gte_nop();
        gte_rtv1_b();
        out[0] = base + color;

        gte_pop_color(color);
        color >>= 3;
        gte_nop();
        gte_rtv2_b();
        out[2] = base + color;

        gte_pop_color(color);
        gte_ldv3c(vertices);
        color >>= 3;
        gte_nop();
        gte_rtv0_b();
        out[4] = base + color;

        vertices += 3;
        out += 6;
    }
}

POLY_GT4 * SubEfct_800CC860(unsigned int *indices, POLY_GT4 *packs, int n_packs)
{
    unsigned int scratch;
    unsigned int n0, n1, n2, n3;

    scratch = SCRPAD_ADDR;

    while (--n_packs >= 0)
    {
        if (packs->tag & 0xFF000000)
        {
            n0 = *indices;
            n1 = *indices;
            n2 = *indices;
            n3 = *indices;

            n0 <<= 1;
            n1 >>= 7;
            n2 >>= 23;
            n3 >>= 15;

            n0 &= 0xFE;
            n1 &= 0xFE;
            n2 &= 0xFE;
            n3 &= 0xFE;

            n0 += scratch;
            n1 += scratch;
            n2 += scratch;
            n3 += scratch;

            packs->u0 = *(u_char *)n0;
            packs->u1 = *(u_char *)n1;
            packs->u2 = *(u_char *)n2;
            packs->u3 = *(u_char *)n3;
        }

        packs++;
        indices++;
    }

    return packs;
}

void SubEfct_800CC914(DG_OBJ *obj)
{
    POLY_GT4 *packs;
    DG_MDL   *model;

    packs = obj->packs[1 - GV_Clock];
    if (packs == NULL)
    {
        return;
    }
#ifdef __psyz
    {
        /* what does the hull actually get drawn with? */
        static int beat;
        if ((++beat % 120) == 1)
        {
            if (beat == 1)
            {
                const DG_MDL *m = obj->model;
                printf("[sub] model bbox %d,%d,%d..%d,%d,%d raise %d n_verts %d n_faces %d flags %08x\n",
                       m->min.vx, m->min.vy, m->min.vz, m->max.vx, m->max.vy, m->max.vz,
                       obj->raise, m->n_verts, m->n_faces, (unsigned)m->flags);
            }
            printf("[sub] ambient %d,%d,%d\n", DG_Ambient.vx, DG_Ambient.vy, DG_Ambient.vz);
            printf("[sub] t %d world %d,%d,%d screen.t %d,%d,%d\n", beat, (int)obj->world.t[0],
                   (int)obj->world.t[1], (int)obj->world.t[2], (int)obj->screen.t[0],
                   (int)obj->screen.t[1], (int)obj->screen.t[2]);
            const POLY_GT4 *p = packs;
            DG_TEX *tex = DG_GetTexture(obj->model->materials[0]);
            int k, tagged = 0;
            for (k = 0; k < obj->n_packs; k++)
            {
                if (packs[k].tag)
                {
                    tagged++;
                }
            }
            printf("[sub] pack0 code %02X rgb0 %d,%d,%d rgb1 %d,%d,%d rgb2 %d,%d,%d tpage %04X clut %04X uv %d,%d %d,%d %d,%d %d,%d | "
                   "xy %d,%d %d,%d %d,%d %d,%d tag %08lX len %lu | material %04X -> tex off %d,%d w %d h %d tpage %04X clut %04X | "
                   "n_packs %d tagged %d verts %d\n",
                   p->code, p->r0, p->g0, p->b0, p->r1, p->g1, p->b1, p->r2, p->g2, p->b2,
                   p->tpage, p->clut, p->u0, p->v0, p->u1, p->v1, p->u2, p->v2, p->u3, p->v3,
                   p->x0, p->y0, p->x1, p->y1, p->x2, p->y2, p->x3, p->y3, (unsigned long)p->tag,
                   (unsigned long)p->len,
                   obj->model->materials[0], tex->off_x, tex->off_y, tex->w, tex->h, tex->tpage,
                   tex->clut, obj->n_packs, tagged, obj->model->n_verts);
        }
    }
#endif

    while (obj != NULL)
    {
        model = obj->model;
        SubEfct_800CC798(model);
        packs = SubEfct_800CC860((unsigned int *)model->vindices, packs, obj->n_packs);
        obj = obj->extend;
    }
}

void SubEfct_800CC9A0(Work *work)
{
    MATRIX   world;
    DG_OBJS *objs;
    DG_OBJ  *obj;
    int      i;

    objs = work->parent->objs;

    world = DG_ZeroMatrix;
    RotMatrixZ(work->rotation->vz, &world);
    DG_SetPos(&world);

    obj = objs->objs;
    for (i = objs->n_models; i > 0; i--)
    {
        SubEfct_800CC914(obj);
        obj++;
    }
}

void SubEfct_800CCA58(Work *work)
{
    DG_OBJS *objs;
    DG_OBJ  *obj;
    int      i;

    objs = work->parent->objs;
    objs->flag = work->flag;

    obj = objs->objs;
    for (i = objs->n_models; i > 0; i--)
    {
        DG_WriteObjPacketUV(obj, 0);
        DG_WriteObjPacketUV(obj, 1);
        obj++;
    }
}

void SubEfctAct_800CCAC0(Work *work)
{
    SubEfct_800CC9A0(work);
    work->parent->light = work->light;
}

void SubEfctDie_800CCAF0(Work *work)
{
    SubEfct_800CCA58(work);
}

void *NewSubEfct_800CCB10(OBJECT *parent, SVECTOR *rotation)
{
    Work *work;
    DG_OBJS     *objs;

    work = GV_NewActor(EXEC_LEVEL, sizeof(Work));
    if (work != NULL)
    {
        GV_SetNamedActor(&work->actor, SubEfctAct_800CCAC0, SubEfctDie_800CCAF0, "sub_efct.c");

        work->parent = parent;
        work->rotation = rotation;

        objs = parent->objs;
        work->flag = objs->flag;

        objs->flag = (objs->flag & ~DG_FLAG_BOUND) | DG_FLAG_AMBIENT | DG_FLAG_GBOUND;

        work->lightp = objs->light;
        objs->light = work->light;

        work->light[0].m[0][0] = 0;
        work->light[0].m[0][1] = -4096;
        work->light[0].m[0][2] = 0;

        work->light[1].m[0][0] = 2048;
        work->light[1].m[1][0] = 2048;
        work->light[1].m[2][0] = 2048;
#ifdef MGS_BRIGHT_SUB
        /* experiment: an unmistakable hull. Full light, grey ambient. */
        work->light[1].m[0][0] = 4096;
        work->light[1].m[1][0] = 4096;
        work->light[1].m[2][0] = 4096;
        work->light[0].t[0] = 160;
        work->light[0].t[1] = 160;
        work->light[0].t[2] = 160;
        printf("[sub] BRIGHT experiment: light x2, ambient 160\n");
#endif
    }

    return (void *)work;
}
