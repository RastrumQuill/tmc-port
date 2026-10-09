/**
 * @file intr.c
 * @brief C translation of the ARM routines that the GBA version copies to
 * IWRAM (asm/src/intr.s): collision detection, the entity update loop,
 * palette fading and the sprite (OAM) builder.
 *
 * The sprite builder is extended for the PC port: it culls against the
 * (possibly enlarged) visible area and records full precision sprite
 * coordinates in gPortOamExtWork.
 */
#include "asm_common.h"
#include "asm_internal.h"
#include "port.h"

#include <setjmp.h>
#include <string.h>

#include "affine.h"
#include "vram.h"

/* ------------------------------------------------------------------------ */
/* collision                                                                */
/* ------------------------------------------------------------------------ */

typedef struct {
    void* last;
    void* first;
    void* node;
    u8 flags;
} LinkedList2;

typedef u32 (*CollisionHandler)(Entity* org, Entity* tgt, u32 direction, void* settings);

extern LinkedList2* gUnk_02018EA0;
extern Entity* gCollidableList[];
extern u8 gCollidableCount;
extern u8 gCollisionMtx[];
extern const CollisionHandler gCollisionHandlers[];
extern const u8 gUnk_0800464E[];
void sub_080044AE(Entity* this, u32 speed, u32 direction);

void UpdateCollision(Entity* this) {
    if (this->flags & 0x80) {
        gCollidableList[gCollidableCount] = this;
        gCollidableCount++;
    }
}

u32 Asm_CalcCollisionDirection(s32 x1, s32 y1, s32 x2, s32 y2) {
    u32 dir;
    s32 dx = x2 - x1;
    s32 dy = y1 - y2;
    u32 ax, ay, lo, hi, step;
    if (dx >= 0) {
        dir = 0;
    } else {
        dir = 0x20;
        dx = -dx;
    }
    if (dy < 0) {
        dy = -dy;
        dir += 0x10;
    }
    ax = dx;
    ay = dy;
    if (ax >= ay) {
        lo = ay;
        hi = ax;
        dir += 8;
    } else {
        lo = ax;
        hi = ay;
    }
    lo <<= 3;
    step = hi * 2;
    if (lo >= hi) {
        dir++;
        hi += step;
    }
    if (lo >= hi) {
        dir++;
        hi += step;
    }
    if (lo >= hi) {
        dir++;
        hi += step;
    }
    if (lo >= hi)
        dir++;
    return gUnk_0800464E[dir];
}

/* reflection helpers (the BBOX_REFLECT jump table) */
static u32 ReflectDirection(u32 calcDir, u32 dir) {
    u32 rel = (calcDir - ((dir - 8) & 0x1F)) & 0x1F;
    if (rel < 0x11)
        return calcDir;
    rel = (calcDir - dir - 8) & 0x1F;
    return (calcDir - rel * 2) & 0x1F;
}

static u32 CalcCollision(Entity* this, Entity* other) {
    Hitbox* bbThis = this->hitbox;
    Hitbox* bbOther = other->hitbox;
    s32 dx, dy, dz;
    u32 w, h, d, dir, solid, idx, handlerIndex, result;
    u8* settings;

    dx = this->x.HALF_U.HI - other->x.HALF_U.HI + bbThis->offset_x - bbOther->offset_x;
    w = bbThis->width + bbOther->width;
    if ((u32)(dx + w) > w * 2)
        return 0;
    dy = this->y.HALF_U.HI - other->y.HALF_U.HI + bbThis->offset_y - bbOther->offset_y;
    h = bbThis->height + bbOther->height;
    if ((u32)(dy + h) > h * 2)
        return 0;
    d = ((this->collisionFlags & 0x10) ? ((u8*)bbThis)[8] : 5) + ((other->collisionFlags & 0x10) ? ((u8*)bbOther)[8] : 5);
    dz = this->z.HALF.HI - other->z.HALF.HI;
    if ((u32)(d + dz) > d * 2)
        return 0;

    solid = ((this->collisionFlags & 0x20) >> 3) + ((other->collisionFlags & 0x20) >> 2);
    switch (solid) {
        case 4: /* BBOX_SOLID this */
            dx = this->x.HALF_U.HI - other->x.HALF_U.HI - bbOther->offset_x;
            dy = this->y.HALF_U.HI - other->y.HALF_U.HI - bbOther->offset_y;
            break;
        case 8: /* BBOX_SOLID other */
            dx = this->x.HALF_U.HI - other->x.HALF_U.HI + bbThis->offset_x;
            dy = this->y.HALF_U.HI - other->y.HALF_U.HI + bbThis->offset_y;
            break;
        case 12: /* BBOX_SOLID both */
            dx = this->x.HALF_U.HI - other->x.HALF_U.HI;
            dy = this->y.HALF_U.HI - other->y.HALF_U.HI;
            break;
    }
    dir = Asm_CalcCollisionDirection(dx, dy, 0, 0);

    idx = other->hitType * 0x22 + this->hurtType;
    settings = gCollisionMtx + idx * 12;
    handlerIndex = 0;
    if (settings[0] == 0xFF)
        handlerIndex = *(u16*)(settings + 2);
    result = gCollisionHandlers[handlerIndex](this, other, dir, settings);
    if (result == 0)
        return 0;
    if (result == 2)
        return 1;

    switch (((this->collisionFlags & 0x80) >> 5) + ((other->collisionFlags & 0x80) >> 4)) {
        case 4: /* BBOX_REFLECT this */
            dir = ReflectDirection(dir, this->direction);
            break;
        case 8: /* BBOX_REFLECT other */
            dir = ReflectDirection(dir, other->direction ^ 0x10);
            break;
        case 12: /* BBOX_REFLECT both */
            dir = this->direction;
            break;
    }
    other->knockbackDirection = dir;
    this->knockbackDirection = dir ^ 0x10;
    return 1;
}

void ram_CollideAll(void) {
    LinkedList2* node;
    LinkedList2* start;
    u32 count = gCollidableCount;
    if (count == 0)
        return;
    node = gUnk_02018EA0;
    start = node;
    do {
        Entity* this = node->node;
        if (this->flags & 0x80) {
            u32 mask = 1 << (this->collisionFlags & 7);
            u8 thisFlags = this->collisionFlags;
            u8 thisIframes = this->iframes;
            s32 i = count;
            while (--i >= 0) {
                Entity* other = gCollidableList[i];
                if (this == other)
                    continue;
                if (!(other->collisionMask & mask))
                    continue;
                if (!(other->flags & 0x80))
                    continue;
                if (!(this->collisionLayer & other->collisionLayer))
                    continue;
                if (!((other->collisionFlags | thisFlags) & 0x40)) {
                    if ((u8)(other->iframes | thisIframes) != 0)
                        continue;
                }
                if (!CalcCollision(this, other))
                    continue;
                if (!(other->contactFlags & 0x80)) {
                    other->contactedEntity = this;
                    other->contactFlags = this->hurtType | 0x80;
                }
                if (!(this->contactFlags & 0x80)) {
                    this->contactedEntity = other;
                    this->contactFlags = other->hurtType | 0x80;
                }
            }
        }
        node = node->first;
    } while (node != start);
}

u32 Asm_CalcCollisionStaticEntity(Entity* target, Entity* origin) {
    Hitbox* tb = target->hitbox;
    Hitbox* ob;
    s32 dx, dy;
    u32 w, h, overlapX, overlapY, dirX, dir;
    if (tb == NULL)
        return 0;
    ob = origin->hitbox;
    if (ob == NULL)
        return 0;
    dx = (origin->x.HALF_U.HI + ob->offset_x) - target->x.HALF_U.HI - tb->offset_x;
    w = tb->width + ob->width;
    if (w * 2 < (u32)(dx + w))
        return 0;
    if (dx >= 0) {
        dirX = 8;
    } else {
        dirX = 0x18;
        dx = -dx;
    }
    overlapX = w - dx;
    dy = (origin->y.HALF_U.HI + ob->offset_y) - target->y.HALF_U.HI - tb->offset_y;
    h = tb->height + ob->height;
    if (h * 2 < (u32)(dy + h))
        return 0;
    if (dy >= 0) {
        dir = 0x10;
    } else {
        dir = 0;
        dy = -dy;
    }
    overlapY = h - dy;
    if (overlapY >= overlapX) {
        overlapY = overlapX;
        dir = dirX;
    }
    if (overlapY == 0)
        return 0;
    if (overlapY >= 5)
        overlapY = 4;
    sub_080044AE(origin, overlapY << 8, dir);
    return 1;
}

/* ------------------------------------------------------------------------ */
/* entity update loop                                                       */
/* ------------------------------------------------------------------------ */

typedef struct {
    void* table;
    void* list_top;
    Entity* current_entity;
    void* restore_sp;
} UpdateContext;

extern UpdateContext gUpdateContext;
extern Entity* gEnemyTarget;
extern LinkedList gEntityLists[9];
extern u8 gCollidableCount;

void DeleteThisEntity(void);
void PlayerUpdate(Entity*);
void EnemyUpdate(Entity*);
void ProjectileUpdate(Entity*);
void ObjectUpdate(Entity*);
void NPCUpdate(Entity*);
void ItemUpdate(Entity*);
void ManagerUpdate(Entity*);

static void DeleteThisEntityWrapper(Entity* e) {
    (void)e;
    DeleteThisEntity();
}

static void (*const sEntityUpdaters[])(Entity*) = {
    DeleteThisEntityWrapper, PlayerUpdate, DeleteThisEntityWrapper, EnemyUpdate, ProjectileUpdate,
    DeleteThisEntityWrapper, ObjectUpdate, NPCUpdate,               ItemUpdate,  ManagerUpdate,
};

typedef struct {
    Entity** enemyTarget;
    u8* first;
    u8* end;
} UpdateTable;

static UpdateTable sUpdateTables[2];
static jmp_buf* sUpdateJump;

/*
 * Updates all entities (which == 0) or all managers (which == 1).
 * An entity that deletes itself calls DeleteThisEntity, which never returns:
 * the original restores the stack pointer and continues with the next entity
 * (ClearAndUpdateEntities). That is done with longjmp here.
 */
void ram_UpdateEntities(u32 which) {
    jmp_buf jump;
    jmp_buf* outer = sUpdateJump;
    UpdateTable* table = &sUpdateTables[which & 1];
    Entity* e;

    table->enemyTarget = &gEnemyTarget;
    if (which & 1) {
        table->first = (u8*)&gEntityLists[7];
        table->end = (u8*)&gCollidableCount;
    } else {
        table->first = (u8*)gEntityLists - 8;
        table->end = (u8*)gEntityLists + 64;
    }
    gUpdateContext.table = table;
    gUpdateContext.list_top = table->first;
    sUpdateJump = &jump;
    if (setjmp(jump) != 0) {
        /* ClearAndUpdateEntities: continue after the deleted entity */
        table = gUpdateContext.table;
        e = gUpdateContext.current_entity;
        goto next;
    }
    for (;;) {
        u8* list = (u8*)gUpdateContext.list_top + 8;
        gUpdateContext.list_top = list;
        if (list >= table->end)
            break;
        e = ((Entity*)list)->next;
        while (e != (Entity*)list) {
            *table->enemyTarget = NULL;
            gUpdateContext.current_entity = e;
            sEntityUpdaters[e->kind](e);
            if (gUpdateContext.current_entity == e)
                UpdateCollision(e);
            e = gUpdateContext.current_entity;
        next:
            e = e->next;
            list = gUpdateContext.list_top;
        }
    }
    gUpdateContext.current_entity = NULL;
    sUpdateJump = outer;
}

void ram_ClearAndUpdateEntities(void) {
    if (sUpdateJump == NULL)
        Port_Fatal("DeleteThisEntity called outside of the entity update loop");
    longjmp(*sUpdateJump, 1);
}

/* ------------------------------------------------------------------------ */
/* palette fading                                                           */
/* ------------------------------------------------------------------------ */

extern const u16* const gUnk_08000F54[][4];

void ram_MakeFadeBuff256(u16* src, u16* dest, u32 fadeStrength, u32 color) {
    u32 add = fadeStrength * color;
    u32 mul = 0x400 - (fadeStrength << 2);
    const u16* const* tables = gUnk_08000F54[*(u8*)0x02000006];
    int i;
    for (i = 0; i < 16; i++) {
        u32 c = (u32)(*src++) << 1;
        u32 r = ((mul * (c & 0x3E) + add) >> 10) & 0x3E;
        u32 g = ((mul * ((c >> 5) & 0x3E) + add) >> 10) & 0x3E;
        u32 b = ((mul * ((c >> 10) & 0x3E) + add) >> 10) & 0x3E;
        *dest++ = *(const u16*)((const u8*)tables[0] + r) | *(const u16*)((const u8*)tables[1] + g) |
                  *(const u16*)((const u8*)tables[2] + b);
    }
}

/* ------------------------------------------------------------------------ */
/* sprite drawing                                                           */
/* ------------------------------------------------------------------------ */

PortOamExt gPortOamExtWork[128];
bool gPortHudSprites;
PortOamExt gPortOamExtLive[128];

#define OAMC ((u8*)&gOAMControls)
/* sprites beyond the 128 OAM entries, in the extended view */
static PortOamExtra sOamExtraWork[PORT_OAM_EXTRA];
static int sOamExtraCount;
PortOamExtra gPortOamExtraLive[PORT_OAM_EXTRA];
int gPortOamExtraLiveCount;

/* called with FlushSprites (src/affine.c), when the game starts a new sprite list */
void Port_OamFlush(void) {
    sOamExtraCount = 0;
}

/* sub_080AE218 (src/vram.c) moved the sprite tiles [start, end) to dest */
void Port_OamMoveTiles(u32 start, u32 end, u32 dest) {
    int i;
    for (i = 0; i < sOamExtraCount; i++) {
        u32 tile = sOamExtraWork[i].attr2 & 0x3FF;
        if (tile >= start && tile < end)
            sOamExtraWork[i].attr2 = (sOamExtraWork[i].attr2 & ~0x3FF) | ((tile - start + dest) & 0x3FF);
    }
}

static bool OamExtraAllowed(void) {
    return gPortScreenWidth > GBA_WIDTH || gPortScreenHeight > GBA_HEIGHT;
}

/* OAM (and the extra entries) can't take more sprites */
static bool OamFull(void) {
    return OAMC[3] >= 0x80 && (!OamExtraAllowed() || sOamExtraCount >= PORT_OAM_EXTRA);
}

extern u8* const gUnk_081326EC[];
extern u32 gFrameObjLists[];
extern u8 gUnk_020000C0[];

/* tables copied to IWRAM from data/const/interrupts.s (unusedLabel_080B2AA8) */
extern const u8* const ram_0x80b2b58[];
extern const u8* const ram_0x80b2bd8[];
extern const u8 ram_0x80b2be8[];

/* The register state of the original routine. */
typedef struct {
    s32 x;           /* r6 */
    s32 y;           /* r7 */
    u32 attr01;      /* r8 */
    u32 attr2;       /* sb */
    const u8* list;  /* sl */
    u8 shadowOffset; /* fp[0x12] */
    u32 tileHi;      /* PC: sprite tiles above the 10 bits of OAM (extra graphics slots) */
} DrawState;

static jmp_buf* sOamFullJump;


/* sub_080B2874: emits the OAM entries of a frame object list. */
static void EmitObjList(DrawState* s) {
    const u8* list = s->list;
    u32 count = *list++;
    const u8* sizeTable;
    u32 index;
    u32 attr2Base;
    u32 tableOffset;

    if (count == 0)
        return;
    attr2Base = s->attr2 & 0xFFFF;
    if (s->attr01 & 0x300) {
        tableOffset = ((s->attr01 & 0x300) == 0x100) ? 0 : 0xC0;
    } else {
        tableOffset = ((s->attr01 & 0x30000000) >> 24) * 3;
    }
    sizeTable = ram_0x80b2be8 + tableOffset;
    index = OAMC[3];

    do {
        s32 ox = (s8)list[0];
        s32 oy = (s8)list[1];
        u32 attr = list[2];
        const u8* size;
        s32 x, y;
        list += 5;
        count--;
        if (!(s->attr01 & 0x300)) {
            if (s->attr01 & 0x20000000)
                oy = -oy;
            if (s->attr01 & 0x10000000)
                ox = -ox;
        }
        size = sizeTable + ((attr & 0xF0) >> 2);
        y = oy + s->y - size[1];
        if (y >= gPortScreenTop + gPortScreenHeight || y + size[3] <= gPortScreenTop)
            continue;
        x = ox + s->x - size[0];
        if (x >= gPortScreenLeft + gPortScreenWidth || x + size[2] <= gPortScreenLeft)
            continue;
        {
            u32 attr01 = (y & 0xFF) | (((u32)x << 23) >> 7) | s->attr01 | ((attr & 0xC0) << 8);
            u32 attr2, tile;
            PortOamExt* ext;
            attr01 ^= (attr & 0x3C) << 26;
            /* The full tile number; OAM keeps its low 10 bits. On the GBA the tile
             * sum never passes 0x3FF, but sprites in the extra graphics slots can:
             * keep the carry out of the priority and palette bits. */
            tile = (attr2Base & 0x3FF) + list[-2] + ((list[-1] & 3) << 8);
            attr2 = attr2Base & ~0x3FF;
            if (attr & 1)
                attr2 &= ~0xF000;
            attr2 += (list[-1] & ~3) << 8;
            tile += s->tileHi;
            attr2 = (attr2 & 0xFC00) | (tile & 0x3FF);

            if (index < 0x80) {
                struct OamData* oam = &gOAMControls.oam[index];
                *(u32*)oam = attr01;
                ((u16*)oam)[2] = (u16)attr2;
                ext = &gPortOamExtWork[index];
            } else {
                /* OAM is full: the extended view continues in the extra list */
                sOamExtraWork[sOamExtraCount].attr2 = (u16)attr2;
                ext = &sOamExtraWork[sOamExtraCount++].ext;
            }
            ext->x = (s16)x;
            ext->y = (s16)y;
            ext->attr0 = (u16)attr01;
            ext->attr1 = (u16)(attr01 >> 16);
            ext->tile = (u16)tile;
            ext->valid = 1;
            ext->anchor = 0;
            if (gPortHudSprites) {
                ext->anchor = PORT_ANCHOR_HUD | (s->x >= GBA_WIDTH / 2 ? PORT_ANCHOR_RIGHT : 0) |
                              (s->y >= GBA_HEIGHT / 2 ? PORT_ANCHOR_BOTTOM : 0);
            }
            if (index < 0x80)
                index++;
            if (index >= 0x80) {
                OAMC[3] = 0x80;
                if (OamFull())
                    longjmp(*sOamFullJump, 1);
            }
        }
    } while (count != 0);
    OAMC[3] = (u8)index;
}

/* sub_080B299C: load the draw state from an entity. */
static void LoadEntityState(DrawState* s, Entity* e) {
    u32 flash = (e->iframes > 0) ? OAMC[0xe] : 0;
    u32 settings = *(u32*)((u8*)e + 0x18);
    s->attr2 = (e->spriteVramOffset & 0x3FF) | (((e->palette.raw | flash) & 0xF) << 12) | ((U8AT(e, 0x1b) & 0xC0) << 4);
    s->tileHi = e->spriteVramOffset & ~0x3FF;
    s->x = e->x.HALF.HI + e->spriteOffsetX;
    s->y = e->y.HALF.HI + e->z.HALF.HI + e->spriteOffsetY;
    if ((settings & 3) != 2) {
        s->x -= (s16)gOAMControls._4;
        s->y -= (s16)gOAMControls._6;
    }
    s->attr01 = (0x3E003F00 & settings) | (OAMC[2] << 12) | (((e->frameSpriteSettings ^ settings) & 0xC0) << 22);
}

/* sub_080B27F4 */
static void DrawSpriteFrame(DrawState* s, u32 frame, u32 spriteIndex) {
    u8* base = (u8*)gFrameObjLists;
    u32* frames = (u32*)(base + gFrameObjLists[spriteIndex]);
    s->list = base + frames[frame];
    EmitObjList(s);
}

/* _080B27E4 */
static void DrawEntityFrame(DrawState* s, Entity* e) {
    if (e->frameIndex == 0xFF)
        return;
    DrawSpriteFrame(s, e->frameIndex, (u16)e->spriteIndex);
}

/* _080B2718 */
static void DrawEntityBody(DrawState* s, Entity* e) {
    s8 mode = (s8)e->spriteAnimation[2];
    u8* part;
    int i;
    if (mode == 0) {
        DrawEntityFrame(s, e);
        return;
    }
    if (mode < 0) {
        s->list = e->myHeap;
        EmitObjList(s);
        return;
    }
    /* multi part sprite */
    part = gUnk_020000C0 + mode * 64;
    s->attr2 &= ~0xF000;
    for (i = 0; i < 4; i++, part += 0x10) {
        u8 flags = part[0];
        if (!(flags & 1))
            return;
        if (flags & 2) {
            Entity* sub = *(Entity**)(part + 0xc);
            if (sub != NULL) {
                /* the original does not restore its registers afterwards, so the
                 * remaining parts continue with the state of the sub entity */
                u8 shadowOffset = s->shadowOffset;
                LoadEntityState(s, sub);
                s->shadowOffset = shadowOffset;
                s->y += shadowOffset;
                DrawEntityFrame(s, sub);
            }
        } else if (part[1] != 0xFF) {
            DrawState p = *s;
            p.attr01 ^= part[4] << 28;
            if (!(p.attr01 & 0x10000000))
                p.x += (s8)part[6];
            else
                p.x -= (s8)part[6];
            p.y += (s8)part[7];
            p.attr2 |= part[5] << 12;
            /* PC: as in EmitObjList, a tile sum past 0x3FF goes to tileHi */
            p.tileHi += ((p.attr2 & 0x3FF) + part[8]) & ~0x3FF;
            p.attr2 = (p.attr2 & ~0x3FF) | (((p.attr2 & 0x3FF) + part[8]) & 0x3FF);
            DrawSpriteFrame(&p, part[1], *(u16*)(part + 2));
        }
    }
}

/* Shadows are collected and drawn after all entities of a priority layer. */
typedef struct {
    s16 x;
    s16 y;
    u8 size;
    u8 priority;
} Shadow;

static Shadow sShadows[0xFF];

/*
 * PC: the larger view can have more than the GBA's 64 entities per priority
 * layer and 64 shadows; the extra ones are kept here instead of being dropped.
 */
#define DRAW_EXTRA 1024
static Entity* sDrawExtra[4][DRAW_EXTRA];
static u32 sDrawExtraCount[4];
static Entity* sDrawMerged[0x40 + DRAW_EXTRA];

/* with the reset of the draw lists in CopyOAM (src/affine.c) */
void Port_DrawListsReset(void) {
    memset(sDrawExtraCount, 0, sizeof(sDrawExtraCount));
}

/* DrawEntity (code_08003FC4.c) when draw list `which` is full */
void Port_DrawListOverflow(u32 which, Entity* e) {
    if (OamExtraAllowed() && sDrawExtraCount[which] < DRAW_EXTRA)
        sDrawExtra[which][sDrawExtraCount[which]++] = e;
}

/* sub_080B255C */
static void DrawEntitySprite(Entity* e) {
    DrawState s;
    s32 z;
    u8 prio;
    u8* shadowList;

    LoadEntityState(&s, e);
    s.shadowOffset = 0;
    OAMC[0x12] = 0;
    if (!(*(u8*)&e->spritePriority & 8)) {
        DrawEntityBody(&s, e);
        return;
    }
    if (e->z.HALF.HI >= 0) {
        u32 actTile = sub_080B1BCC(e, 0, 0);
        u32 shadowSize = U8AT(e, 0x18) & 0x30;
        u32 overlay = 0;
        bool32 drawOverlay = FALSE;
        if (actTile == 0x19) {
            s.shadowOffset = 2;
            s.y += 2;
        } else if (actTile == 0x2f) {
            overlay = (U8AT(e, 0x2e) ^ U8AT(e, 0x32)) & 6;
            drawOverlay = TRUE;
        } else if (actTile == 0xf) {
            s.shadowOffset = 2;
            s.y += 2;
            overlay = ((OAMC[1] & 0x18) + 0x80) >> 2;
            drawOverlay = TRUE;
        }
        OAMC[0x12] = s.shadowOffset;
        if (drawOverlay) {
            /* grass / water overlay drawn in front of the entity */
            DrawState o = s;
            o.list = *(const u8* const*)((const u8*)ram_0x80b2b58 + shadowSize + overlay * 2);
            o.attr01 = 0;
            o.attr2 &= 0xC00;
            o.tileHi = 0;
            EmitObjList(&o);
            DrawEntityBody(&s, e);
            return;
        }
    }
    DrawEntityBody(&s, e);

    z = e->z.HALF.HI;
    prio = *(u8*)&e->spritePriority;
    if ((prio & 0x10) && z >= 0)
        return;
    if ((prio & 0x20) && (OAMC[1] & 1))
        return;
    shadowList = gUnk_081326EC[4];
    if (shadowList[0] >= (OamExtraAllowed() ? 0xFF : 0x40))
        return;
    {
        Shadow* sh = &sShadows[shadowList[0]];
        shadowList[0]++;
        sh->x = (s16)s.x;
        sh->y = (s16)(z < 0 ? s.y - z : s.y);
        sh->size = (U8AT(e, 0x18) & 0x30) >> 4;
        sh->priority = (s.attr2 >> 10) & 3;
    }
}

static u32 DrawKey(Entity* e) {
    return ((u32)(e->y.WORD + 0x80000000) >> 3) | ((~(u32)*(u8*)&e->spritePriority) << 29);
}

/* Gapped insertion sort of the draw list (same algorithm as the original, keeps the exact order). */
static void ResolveOamDrawPriority(Entity** first, u32 count) {
    s32 gap = count - 1;
    Entity** last = first + gap;
    if (gap == 0)
        return;
    do {
        s32 j;
        for (j = 0; j < gap; j++) {
            Entity** p = last - gap - j;
            for (; p >= first; p -= gap) {
                Entity* a = *p;
                u32 keyA = DrawKey(a);
                Entity** q = p + gap;
                while (q <= last) {
                    Entity* b = *q;
                    if (keyA >= DrawKey(b))
                        break;
                    q[-gap] = b;
                    q += gap;
                }
                q[-gap] = a;
            }
        }
        gap >>= 1;
    } while (gap != 0);
}

static void DrawShadows(void) {
    u8* shadowList = gUnk_081326EC[4];
    u32 n = shadowList[0];
    u32 i;
    for (i = 0; i < n; i++) {
        DrawState s;
        s.x = sShadows[i].x;
        s.y = sShadows[i].y;
        s.list = ram_0x80b2bd8[sShadows[i].size & 7];
        s.attr2 = sShadows[i].priority << 10;
        s.attr01 = 0;
        s.shadowOffset = 0;
        s.tileHi = 0;
        EmitObjList(&s);
    }
}

static void DrawList(u32 which) {
    u8* list = gUnk_081326EC[which];
    u32 i, n = list[0], extra = sDrawExtraCount[which];
    Entity** ents = (Entity**)(list + 4);
    if (n == 0)
        return;
    gUnk_081326EC[4][0] = 0;
    if (extra != 0) {
        /* the list as if it were longer */
        memcpy(sDrawMerged, ents, n * sizeof(Entity*));
        memcpy(sDrawMerged + n, sDrawExtra[which], extra * sizeof(Entity*));
        ents = sDrawMerged;
        n += extra;
    }
    ResolveOamDrawPriority(ents, n);
    for (i = 0; i < n; i++)
        DrawEntitySprite(ents[i]);
    DrawShadows();
}

void ram_DrawEntities(void) {
    jmp_buf jump;
    jmp_buf* outer = sOamFullJump;
    if (OamFull())
        return;
    sOamFullJump = &jump;
    if (setjmp(jump) == 0) {
        DrawList(0);
        DrawList(1);
        DrawList(2);
        DrawList(3);
    }
    sOamFullJump = outer;
}

void ram_sub_080ADA04(OAMCommand* cmd, void* objList) {
    jmp_buf jump;
    jmp_buf* outer;
    DrawState s;
    if (((u8*)objList)[0] == 0)
        return;
    if (OamFull())
        return;
    s.list = objList;
    s.x = cmd->x;
    s.y = cmd->y;
    s.attr01 = *(u32*)&cmd->_4;
    s.attr2 = cmd->_8;
    s.shadowOffset = 0;
    s.tileHi = 0;
    outer = sOamFullJump;
    sOamFullJump = &jump;
    if (setjmp(jump) == 0)
        EmitObjList(&s);
    sOamFullJump = outer;
}

void ram_DrawDirect(OAMCommand* cmd, u32 spriteIndex, u32 frameIndex) {
    u8* base = (u8*)gFrameObjLists;
    u32* frames;
    if (frameIndex == 0xFF)
        return;
    frames = (u32*)(base + gFrameObjLists[spriteIndex]);
    ram_sub_080ADA04(cmd, base + frames[frameIndex]);
}

void Port_OnOamCopy(const void* src, void* dest, uint32_t bytes) {
    if ((uintptr_t)dest == PORT_OAM_ADDR && src == (const void*)gOAMControls.oam && bytes >= PORT_OAM_SIZE) {
        memcpy(gPortOamExtLive, gPortOamExtWork, sizeof(gPortOamExtLive));
        memcpy(gPortOamExtraLive, sOamExtraWork, sOamExtraCount * sizeof(PortOamExtra));
        gPortOamExtraLiveCount = sOamExtraCount;
    }
}

/* savestates (port/src/debug.c) */
void* Intr_StateData(size_t* size) {
    *size = sizeof(sUpdateTables);
    return sUpdateTables;
}
