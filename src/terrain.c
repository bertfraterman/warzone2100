/*
	This file is part of Warzone 2100.
	Copyright (C) 2008  Warzone Resurrection Project

	Warzone 2100 is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	Warzone 2100 is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Warzone 2100; if not, write to the Free Software
	Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
*/

/**
 * @file terrain.c
 * Draws the terrain
 */

#include "lib/ivis_opengl/GLee.h"
#include <string.h>
#include <SDL_video.h>

#include "lib/framework/frame.h"
#include "lib/ivis_common/ivisdef.h"
#include "lib/ivis_common/imd.h"
#include "lib/ivis_common/rendmode.h"
#include "lib/ivis_common/piefunc.h"
#include "lib/ivis_common/tex.h"
#include "lib/ivis_common/piedef.h"
#include "lib/ivis_common/piestate.h"
#include "lib/ivis_common/pieclip.h"

#include "terrain.h"
#include "map.h"
#include "texture.h"
#include "environ.h"
#include "display3d.h"
#include "hci.h"
#include "loop.h"

typedef struct
{
	int geometryOffset;
	int geometrySize;
	int geometryIndexOffset;
	int geometryIndexSize;
	int waterOffset;
	int waterSize;
	int waterIndexOffset;
	int waterIndexSize;
	int *textureOffset;
	int *textureSize;
	int *textureIndexOffset;
	int *textureIndexSize;
	int decalOffset;
	int decalSize;
	bool draw;
	bool dirty;
} Sector;

typedef struct
{
	GLfloat x, y, z;        // Vertex
} RenderVertex;

typedef struct
{
	GLfloat x, y, z;
	GLfloat u, v;          // uv
} DecalVertex;

static GLuint lightmap_tex_num;
static unsigned int lightmapNextUpdate;
static int lightmapSize;

static GLuint geometryVBO, geometryIndexVBO, textureVBO, textureIndexVBO, decalVBO;
static GLuint waterVBO, waterIndexVBO;
static float waterOffset;

static GLint GLmaxElementsVertices, GLmaxElementsIndices;

static Sector *sectors;
static int sectorSize = 15;
static int terrainDistance;
static int xSectors, ySectors;

bool terrainInitalised = false;

#ifdef DEBUG
/// Check for OpenGL errors 
#define glError() { \
        GLenum err = glGetError(); \
        while (err != GL_NO_ERROR) { \
                debug(LOG_ERROR, "OpenGL error %i caught at %s:%u\n", err, __FILE__, __LINE__); \
                err = glGetError(); \
        } \
}
#else
#define glError()
#endif

/// Helper to specify the offset in a VBO
#define BUFFER_OFFSET(i) ((char *)NULL + (i))

GLuint dreStart, dreEnd, dreOffset;
GLsizei dreCount;
bool drawRangeElementsStarted = false;

static void finishDrawRangeElements(void)
{
	if (drawRangeElementsStarted && dreCount > 0)
	{
		ASSERT(dreEnd - dreStart + 1 <= GLmaxElementsVertices, "too many vertices (%i)", dreEnd - dreStart + 1);
		ASSERT(dreCount <= GLmaxElementsIndices, "too many indices (%i)", dreCount);
		glDrawRangeElements(GL_TRIANGLES,
							dreStart,
							dreEnd,
							dreCount,
							GL_UNSIGNED_INT,
							BUFFER_OFFSET(sizeof(GLuint)*dreOffset));
	}
	drawRangeElementsStarted = false;
}

static void addDrawRangeElements(GLenum mode,
                                 GLuint start, 
                                 GLuint end, 
                                 GLsizei count, 
                                 GLenum type, 
                                 GLuint offset)
{
	ASSERT(mode == GL_TRIANGLES, "not supported");
	ASSERT(type == GL_UNSIGNED_INT, "not supported");
	
	if (end - start + 1 > GLmaxElementsVertices)
	{
		debug(LOG_WARNING, "A single call provided too much vertices, will operate at reduced performance or crash. Decrease the sector size to fix this.");
	}
	if (count > GLmaxElementsIndices)
	{
		debug(LOG_WARNING, "A single call provided too much indices, will operate at reduced performance or crash. Decrease the sector size to fix this.");
	}
	
	if (!drawRangeElementsStarted)
	{
		dreStart  = start;
		dreEnd    = end;
		dreCount  = count;
		dreOffset = offset;
		drawRangeElementsStarted = true;
		return;
	}
	
	// check if we can append theoretically and 
	// check if this will not go over the bounds advised by the opengl implementation
	if (dreOffset + dreCount != offset ||
		dreCount + count > GLmaxElementsIndices ||
		end - dreStart + 1 > GLmaxElementsVertices)
	{
		finishDrawRangeElements();
		// start anew
		addDrawRangeElements(mode, start, end, count, type, offset);
	}
	else
	{
		// OK to append
		dreCount += count;
		dreEnd = end;
	}
	// make sure we did everything right
	ASSERT(dreEnd - dreStart + 1 <= GLmaxElementsVertices, "too many vertices (%i)", dreEnd - dreStart + 1);
	ASSERT(dreCount <= GLmaxElementsIndices, "too many indices (%i)", dreCount);
}

/// Get the colour of the terrain tile at the specified position
PIELIGHT getTileColour(int x, int y)
{
	return mapTile(x, y)->colour;
}
/// Set the colour of the tile at the specified position
void setTileColour(int x, int y, PIELIGHT colour)
{
	MAPTILE *psTile = mapTile(x, y);

	psTile->colour = colour;
}

/// Set up the texture coordinates for a tile
static void getTileTexCoords(Vector2f *uv, unsigned int tileNumber)
{
	/* unmask proper values from compressed data */
	const unsigned short texture = TileNumber_texture(tileNumber);
	const unsigned short tile = TileNumber_tile(tileNumber);

	/* Used to calculate texture coordinates */
	const float xMult = 1.0f / TILES_IN_PAGE_COLUMN;
	const float yMult = 1.0f / TILES_IN_PAGE_ROW;
	const float one = 1.0f / (TILES_IN_PAGE_COLUMN * (float)getTextureSize());

	/*
	 * Points for flipping the texture around if the tile is flipped or rotated
	 * Store the source rect as four points
	 */
	Vector2f
		sP1 = { one, one },
		sP2 = { xMult - one, one },
		sP3 = { xMult - one, yMult - one },
		sP4 = { one, yMult - one },
		sPTemp;

	if (texture & TILE_XFLIP)
	{
		sPTemp = sP1;
		sP1 = sP2;
		sP2 = sPTemp;

		sPTemp = sP3;
		sP3 = sP4;
		sP4 = sPTemp;
	}
	if (texture & TILE_YFLIP)
	{
		sPTemp = sP1;
		sP1 = sP4;
		sP4 = sPTemp;
		sPTemp = sP2;
		sP2 = sP3;
		sP3 = sPTemp;
	}

	switch ((texture & TILE_ROTMASK) >> TILE_ROTSHIFT)
	{
		case 1:
			sPTemp = sP1;
			sP1 = sP4;
			sP4 = sP3;
			sP3 = sP2;
			sP2 = sPTemp;
			break;
		case 2:
			sPTemp = sP1;
			sP1 = sP3;
			sP3 = sPTemp;
			sPTemp = sP4;
			sP4 = sP2;
			sP2 = sPTemp;
			break;
		case 3:
			sPTemp = sP1;
			sP1 = sP2;
			sP2 = sP3;
			sP3 = sP4;
			sP4 = sPTemp;
			break;
	}
	uv[0 + 0].x = tileTexInfo[tile].uOffset + sP1.x;
	uv[0 + 0].y = tileTexInfo[tile].vOffset + sP1.y;
	
	uv[0 + 2].x = tileTexInfo[tile].uOffset + sP2.x;
	uv[0 + 2].y = tileTexInfo[tile].vOffset + sP2.y;
	
	uv[1 + 2].x = tileTexInfo[tile].uOffset + sP3.x;
	uv[1 + 2].y = tileTexInfo[tile].vOffset + sP3.y;
	
	uv[1 + 0].x = tileTexInfo[tile].uOffset + sP4.x;
	uv[1 + 0].y = tileTexInfo[tile].vOffset + sP4.y;
}

/// Average the four positions to get the center
static void averagePos(Vector3i *center, Vector3i *a, Vector3i *b, Vector3i *c, Vector3i *d)
{
	center->x = (a->x + b->x + c->x + d->x)/4;
	center->y = (a->y + b->y + c->y + d->y)/4;
	center->z = (a->z + b->z + c->z + d->z)/4;
}

/// Is this position next to a water tile?
static bool isWater(int x, int y)
{
	bool result = false;
	result = result || (tileOnMap(x  ,y  ) && mapTile(x  ,y  )->ground == waterGroundType);
	result = result || (tileOnMap(x+1,y  ) && mapTile(x+1,y  )->ground == waterGroundType);
	result = result || (tileOnMap(x  ,y+1) && mapTile(x  ,y+1)->ground == waterGroundType);
	result = result || (tileOnMap(x+1,y+1) && mapTile(x+1,y+1)->ground == waterGroundType);
	return result;
}

/// Get the position of a grid point
static void getGridPos(Vector3i *result, int x, int y, bool center, bool water)
{
	if (center)
	{
		Vector3i a,b,c,d;
		getGridPos(&a, x  , y  , false, water);
		getGridPos(&b, x+1, y  , false, water);
		getGridPos(&c, x  , y+1, false, water);
		getGridPos(&d, x+1, y+1, false, water);
		averagePos(result, &a, &b, &c, &d);
		return;
	}
	result->x = world_coord(x);
	result->z = world_coord(-y);
	
	if (x <= 0 || y <= 0 || x >= mapWidth || y >= mapHeight)
	{
		result->y = 0;
	}
	else
	{
		result->y = map_TileHeight_new(x, y);
		if (water)
		{
			result->y = map_WaterHeight(x, y);
		}
	}
}

static inline void averageUV(Vector2f *center, Vector2f* uv)
{
	center->x = (uv[0].x+uv[1].x+uv[2].x+uv[3].x)/4;
	center->y = (uv[0].y+uv[1].y+uv[2].y+uv[3].y)/4;
}

/// Get the texture coordinates for the map position
static inline void getTexCoords(Vector2f *uv, float x, float y, int groundType)
{
	uv->x = (x/psGroundTypes[groundType].textureSize);
	uv->y = (y/psGroundTypes[groundType].textureSize);
}

static inline void averageColour(PIELIGHT *average, PIELIGHT a, PIELIGHT b,
                                                            PIELIGHT c, PIELIGHT d)
{
	average->byte.a = (a.byte.a + b.byte.a + c.byte.a + d.byte.a)/4;
	average->byte.r = (a.byte.r + b.byte.r + c.byte.r + d.byte.r)/4;
	average->byte.g = (a.byte.g + b.byte.g + c.byte.g + d.byte.g)/4;
	average->byte.b = (a.byte.b + b.byte.b + c.byte.b + d.byte.b)/4;
}

/// Get the tiles colour
static inline void getColour(PIELIGHT *colour, int x, int y, bool center)
{
	MAPTILE *psTile;
	
	if (center)
	{
		PIELIGHT a,b,c,d;
		getColour(&a, x  , y  , false);
		getColour(&b, x+1, y  , false);
		getColour(&c, x  , y+1, false);
		getColour(&d, x+1, y+1, false);
		averageColour(colour, a, b, c, d);
		return;
	}
	
	*colour = WZCOL_BLACK;
	if (x < 0 || y < 0 || x > mapWidth - 1 || y > mapHeight - 1)
	{
		return;
	}
	psTile = mapTile(x, y);
	*colour = psTile->colour;

	if (psTile->tileInfoBits & BITS_GATEWAY && showGateways)
	{
		colour->byte.g = 255;
	}
	
	/* Is the tile highlighted? Perhaps because there's a building foundation on it */
	if (TileIsHighlighted(psTile))
	{
		if (outlineTile)
		{
			*colour = pal_SetBrightness(255);
		}
		else
		{
			colour->byte.r = 255;
		}
	}
}

static void setSectorGeometry(int x, int y,
                             RenderVertex *geometry, RenderVertex *water,
                             int *geometrySize, int *waterSize)
{
	Vector3i pos;
	int i,j;
	for (i = 0; i < sectorSize+1; i++)
	{
		for (j = 0; j < sectorSize+1; j++)
		{
			// set up geometry
			getGridPos(&pos, i+x*sectorSize, j+y*sectorSize, false, false);
			geometry[*geometrySize].x = pos.x;
			geometry[*geometrySize].y = pos.y;
			geometry[*geometrySize].z = pos.z;
			(*geometrySize)++;

			getGridPos(&pos, i+x*sectorSize, j+y*sectorSize, true, false);
			geometry[*geometrySize].x = pos.x;
			geometry[*geometrySize].y = pos.y;
			geometry[*geometrySize].z = pos.z;
			(*geometrySize)++;
			
			getGridPos(&pos, i+x*sectorSize, j+y*sectorSize, false, true);
			water[*waterSize].x = pos.x;
			water[*waterSize].y = pos.y;
			water[*waterSize].z = pos.z;
			(*waterSize)++;

			getGridPos(&pos, i+x*sectorSize, j+y*sectorSize, true, true);
			water[*waterSize].x = pos.x;
			water[*waterSize].y = pos.y;
			water[*waterSize].z = pos.z;
			(*waterSize)++;
		}
	}
}

static void setSectorDecals(int x, int y,
                            DecalVertex *decaldata,
                            int *decalSize)
{
	Vector3i pos;
	Vector2f uv[2][2], center;
	int a,b;
	int i,j;

	for (i = x*sectorSize; i < x*sectorSize+sectorSize; i++)
	{
		for (j = y*sectorSize; j < y*sectorSize+sectorSize; j++)
		{
			if (i < 0 || j < 0 || i >= mapWidth || j >= mapHeight)
			{
				continue;
			}
			if (mapTile(i,j)->decal)
			{
				getTileTexCoords(*uv, mapTile(i,j)->texture);
				averageUV(&center, *uv);
				
				getGridPos(&pos, i, j, true, false);
				decaldata[*decalSize].x = pos.x;
				decaldata[*decalSize].y = pos.y;
				decaldata[*decalSize].z = pos.z;
				decaldata[*decalSize].u = center.x;
				decaldata[*decalSize].v = center.y;
				(*decalSize)++;
				a = 0; b = 1;
				getGridPos(&pos, i+a, j+b, false, false);
				decaldata[*decalSize].x = pos.x;
				decaldata[*decalSize].y = pos.y;
				decaldata[*decalSize].z = pos.z;
				decaldata[*decalSize].u = uv[a][b].x;
				decaldata[*decalSize].v = uv[a][b].y;
				(*decalSize)++;
				a = 0; b = 0;
				getGridPos(&pos, i+a, j+b, false, false);
				decaldata[*decalSize].x = pos.x;
				decaldata[*decalSize].y = pos.y;
				decaldata[*decalSize].z = pos.z;
				decaldata[*decalSize].u = uv[a][b].x;
				decaldata[*decalSize].v = uv[a][b].y;
				(*decalSize)++;
				
				getGridPos(&pos, i, j, true, false);
				decaldata[*decalSize].x = pos.x;
				decaldata[*decalSize].y = pos.y;
				decaldata[*decalSize].z = pos.z;
				decaldata[*decalSize].u = center.x;
				decaldata[*decalSize].v = center.y;
				(*decalSize)++;
				a = 1; b = 1;
				getGridPos(&pos, i+a, j+b, false, false);
				decaldata[*decalSize].x = pos.x;
				decaldata[*decalSize].y = pos.y;
				decaldata[*decalSize].z = pos.z;
				decaldata[*decalSize].u = uv[a][b].x;
				decaldata[*decalSize].v = uv[a][b].y;
				(*decalSize)++;
				a = 0; b = 1;
				getGridPos(&pos, i+a, j+b, false, false);
				decaldata[*decalSize].x = pos.x;
				decaldata[*decalSize].y = pos.y;
				decaldata[*decalSize].z = pos.z;
				decaldata[*decalSize].u = uv[a][b].x;
				decaldata[*decalSize].v = uv[a][b].y;
				(*decalSize)++;

				getGridPos(&pos, i, j, true, false);
				decaldata[*decalSize].x = pos.x;
				decaldata[*decalSize].y = pos.y;
				decaldata[*decalSize].z = pos.z;
				decaldata[*decalSize].u = center.x;
				decaldata[*decalSize].v = center.y;
				(*decalSize)++;
				a = 1; b = 0;
				getGridPos(&pos, i+a, j+b, false, false);
				decaldata[*decalSize].x = pos.x;
				decaldata[*decalSize].y = pos.y;
				decaldata[*decalSize].z = pos.z;
				decaldata[*decalSize].u = uv[a][b].x;
				decaldata[*decalSize].v = uv[a][b].y;
				(*decalSize)++;
				a = 1; b = 1;
				getGridPos(&pos, i+a, j+b, false, false);
				decaldata[*decalSize].x = pos.x;
				decaldata[*decalSize].y = pos.y;
				decaldata[*decalSize].z = pos.z;
				decaldata[*decalSize].u = uv[a][b].x;
				decaldata[*decalSize].v = uv[a][b].y;
				(*decalSize)++;

				getGridPos(&pos, i, j, true, false);
				decaldata[*decalSize].x = pos.x;
				decaldata[*decalSize].y = pos.y;
				decaldata[*decalSize].z = pos.z;
				decaldata[*decalSize].u = center.x;
				decaldata[*decalSize].v = center.y;
				(*decalSize)++;
				a = 0; b = 0;
				getGridPos(&pos, i+a, j+b, false, false);
				decaldata[*decalSize].x = pos.x;
				decaldata[*decalSize].y = pos.y;
				decaldata[*decalSize].z = pos.z;
				decaldata[*decalSize].u = uv[a][b].x;
				decaldata[*decalSize].v = uv[a][b].y;
				(*decalSize)++;
				a = 1; b = 0;
				getGridPos(&pos, i+a, j+b, false, false);
				decaldata[*decalSize].x = pos.x;
				decaldata[*decalSize].y = pos.y;
				decaldata[*decalSize].z = pos.z;
				decaldata[*decalSize].u = uv[a][b].x;
				decaldata[*decalSize].v = uv[a][b].y;
				(*decalSize)++;
			}
		}
	}
}

static void updateSectorGeometry(int x, int y)
{
	RenderVertex *geometry;
	RenderVertex *water;
	DecalVertex *decaldata;
	int geometrySize = 0;
	int waterSize = 0;
	int decalSize = 0;
	
	geometry  = malloc(sizeof(RenderVertex)*sectors[x*ySectors + y].geometrySize);
	water     = malloc(sizeof(RenderVertex)*sectors[x*ySectors + y].waterSize);

	setSectorGeometry(x, y, geometry, water, &geometrySize, &waterSize);
	ASSERT(geometrySize == sectors[x*ySectors + y].geometrySize, "something went seriously wrong updating the terrain");
	ASSERT(waterSize    == sectors[x*ySectors + y].waterSize   , "something went seriously wrong updating the terrain");
	
	glBindBuffer(GL_ARRAY_BUFFER, geometryVBO); glError();
	glBufferSubData(GL_ARRAY_BUFFER, sizeof(RenderVertex)*sectors[x*ySectors + y].geometryOffset,
	                                 sizeof(RenderVertex)*sectors[x*ySectors + y].geometrySize, geometry); glError();
	glBindBuffer(GL_ARRAY_BUFFER, waterVBO); glError();
	glBufferSubData(GL_ARRAY_BUFFER, sizeof(RenderVertex)*sectors[x*ySectors + y].waterOffset,
	                                 sizeof(RenderVertex)*sectors[x*ySectors + y].waterSize, water); glError();

	free(geometry);
	free(water);
	
	decaldata = malloc(sizeof(DecalVertex)*sectors[x*ySectors + y].decalSize);
	setSectorDecals(x, y, decaldata, &decalSize);
	ASSERT(decalSize == sectors[x*ySectors + y].decalSize   , "the amount of decals has changed");
	
	glBindBuffer(GL_ARRAY_BUFFER, decalVBO); glError();
	glBufferSubData(GL_ARRAY_BUFFER, sizeof(DecalVertex)*sectors[x*ySectors + y].decalOffset,
	                                 sizeof(DecalVertex)*sectors[x*ySectors + y].decalSize, decaldata); glError();

	free (decaldata);
}

void markTileDirty(int i, int j)
{
	int x, y;
	
	if (!terrainInitalised)
	{
		return; // will be updated anyway
	}
	
	x = i/sectorSize;
	y = j/sectorSize;
	if (x < xSectors && y < ySectors) // could be on the lower or left edge of the map
	{
		sectors[x*ySectors + y].dirty = true;
	}
	
	// it could be on an edge, so update for all sectors it is in
	if (x*sectorSize == i && x > 0)
	{
		if (x-1 < xSectors && y < ySectors)
		{
			sectors[(x-1)*ySectors + y].dirty = true;
		}
	}
	if (y*sectorSize == j && y > 0)
	{
		if (x < xSectors && y-1 < ySectors)
		{
			sectors[x*ySectors + (y-1)].dirty = true;
		}
	}
	if (x*sectorSize == i && x > 0 && y*sectorSize == j && y > 0)
	{
		if (x-1 < xSectors && y-1 < ySectors)
		{
			sectors[(x-1)*ySectors + (y-1)].dirty = true;
		}
	}
}

bool initTerrain(void)
{
	int i, j, x, y, a, b, absX, absY;
	PIELIGHT colour[2][2], centerColour;
	int layer = 0;
	
	RenderVertex *geometry;
	RenderVertex *water;
	DecalVertex *decaldata;
	int geometrySize, geometryIndexSize;
	int waterSize, waterIndexSize;
	int textureSize, textureIndexSize;
	GLuint *geometryIndex;
	GLuint *waterIndex;
	GLuint *textureIndex;
	PIELIGHT *texture;
	int decalSize;
	int size;
	int maxSectorSizeIndices, maxSectorSizeVertices;
	bool decreasedSize = false;
	
	// this information is useful to prevent crashes with buggy opengl implementations
	glGetIntegerv(GL_MAX_ELEMENTS_VERTICES, &GLmaxElementsVertices);
	glGetIntegerv(GL_MAX_ELEMENTS_INDICES,  &GLmaxElementsIndices);
	// testing for crappy cards
	//GLmaxElementsVertices = 1024;
	//GLmaxElementsIndices = 1024;
	debug(LOG_TERRAIN, "GL_MAX_ELEMENTS_VERTICES: %i", GLmaxElementsVertices);
	debug(LOG_TERRAIN, "GL_MAX_ELEMENTS_INDICES:  %i", GLmaxElementsIndices);
	
	// now we know these values, determine the maximum sector size achievable
	maxSectorSizeVertices = sqrt(GLmaxElementsVertices/2)-1;
	maxSectorSizeIndices = sqrt(GLmaxElementsIndices/12);

	debug(LOG_TERRAIN, "preferred sector size: %i", sectorSize);
	debug(LOG_TERRAIN, "maximum sector size due to vertices: %i", maxSectorSizeVertices);
	debug(LOG_TERRAIN, "maximum sector size due to indices: %i", maxSectorSizeIndices);
	
	if (sectorSize > maxSectorSizeVertices)
	{
		sectorSize = maxSectorSizeVertices;
		decreasedSize = true;
	}
	if (sectorSize > maxSectorSizeIndices)
	{
		sectorSize = maxSectorSizeIndices;
		decreasedSize = true;
	}
	if (decreasedSize)
	{
		if (sectorSize < 1)
		{
			debug(LOG_WARNING, "GL_MAX_ELEMENTS_VERTICES: %i", GLmaxElementsVertices);
			debug(LOG_WARNING, "GL_MAX_ELEMENTS_INDICES:  %i", GLmaxElementsIndices);
			debug(LOG_WARNING, "maximum sector size due to vertices: %i", maxSectorSizeVertices);
			debug(LOG_WARNING, "maximum sector size due to indices: %i", maxSectorSizeIndices);
			debug(LOG_ERROR, "Your graphics card and/or drivers do not seem to support glDrawRangeElements, needed for the terrain renderer.");
			debug(LOG_ERROR, "- Do other 3D games work?");
			debug(LOG_ERROR, "- Did you install the latest drivers correctly?");
			debug(LOG_ERROR, "- Do you have a 3D window manager (Aero/Compiz) running?");
			return false;
		}
		debug(LOG_WARNING, "decreasing sector size to %i to fit graphics card constraints", sectorSize);
	}
	
	terrainDistance = 1.5*((visibleTiles.x+visibleTiles.y)/4+sectorSize/2);
	debug(LOG_TERRAIN, "visible tiles x:%i y: %i", visibleTiles.x, visibleTiles.y);
	debug(LOG_TERRAIN, "terrain view distance: %i", terrainDistance);
	
#define o(i,j,center) (((i)*mapHeight+(j))*2+(center))

	/////////////////////
	// Create the sectors
	xSectors = (mapWidth +sectorSize-1)/sectorSize;
	ySectors = (mapHeight+sectorSize-1)/sectorSize;
	sectors = malloc(sizeof(Sector)*xSectors*ySectors);
	
	////////////////////
	// fill the geometry part of the sectors
	geometry = malloc(sizeof(RenderVertex)*xSectors*ySectors*(sectorSize+1)*(sectorSize+1)*2);
	geometryIndex = malloc(sizeof(GLuint)*xSectors*ySectors*sectorSize*sectorSize*12);
	geometrySize = 0;
	geometryIndexSize = 0;
	
	water = malloc(sizeof(RenderVertex)*xSectors*ySectors*(sectorSize+1)*(sectorSize+1)*2);
	waterIndex = malloc(sizeof(GLuint)*xSectors*ySectors*sectorSize*sectorSize*12);
	waterSize = 0;
	waterIndexSize = 0;
	for (x = 0; x < xSectors; x++)
	{
		for (y = 0; y < ySectors; y++)
		{
			sectors[x*ySectors + y].dirty = false;
			sectors[x*ySectors + y].geometryOffset = geometrySize;
			sectors[x*ySectors + y].geometrySize = 0;
			sectors[x*ySectors + y].waterOffset = waterSize;
			sectors[x*ySectors + y].waterSize = 0;
			
			setSectorGeometry(x, y, geometry, water, &geometrySize, &waterSize);
			
			sectors[x*ySectors + y].geometrySize = geometrySize - sectors[x*ySectors + y].geometryOffset;
			sectors[x*ySectors + y].waterSize = waterSize - sectors[x*ySectors + y].waterOffset;
			// and do the index buffers
			sectors[x*ySectors + y].geometryIndexOffset = geometryIndexSize;
			sectors[x*ySectors + y].geometryIndexSize = 0;
			sectors[x*ySectors + y].waterIndexOffset = waterIndexSize;
			sectors[x*ySectors + y].waterIndexSize = 0;
			
			for (i = 0; i < sectorSize; i++)
			{
				for (j = 0; j < sectorSize; j++)
				{
					if (x*sectorSize+i >= mapWidth || y*sectorSize+j >= mapHeight)
					{
						continue; // off map, so skip
					}
#define q(i,j,center) ((x*ySectors+y)*(sectorSize+1)*(sectorSize+1)*2 + ((i)*(sectorSize+1)+(j))*2+(center))
					geometryIndex[geometryIndexSize+0]  = q(i  ,j  ,1);
					geometryIndex[geometryIndexSize+1]  = q(i  ,j  ,0);
					geometryIndex[geometryIndexSize+2]  = q(i+1,j  ,0);
					
					geometryIndex[geometryIndexSize+3]  = q(i  ,j  ,1);
					geometryIndex[geometryIndexSize+4]  = q(i  ,j+1,0);
					geometryIndex[geometryIndexSize+5]  = q(i  ,j  ,0);
					
					geometryIndex[geometryIndexSize+6]  = q(i  ,j  ,1);
					geometryIndex[geometryIndexSize+7]  = q(i+1,j+1,0);
					geometryIndex[geometryIndexSize+8]  = q(i  ,j+1,0);
					
					geometryIndex[geometryIndexSize+9]  = q(i  ,j  ,1);
					geometryIndex[geometryIndexSize+10] = q(i+1,j  ,0);
					geometryIndex[geometryIndexSize+11] = q(i+1,j+1,0);
					geometryIndexSize += 12;
					if (isWater(i+x*sectorSize,j+y*sectorSize))
					{
						waterIndex[waterIndexSize+0]  = q(i  ,j  ,1);
						waterIndex[waterIndexSize+1]  = q(i  ,j  ,0);
						waterIndex[waterIndexSize+2]  = q(i+1,j  ,0);
						
						waterIndex[waterIndexSize+3]  = q(i  ,j  ,1);
						waterIndex[waterIndexSize+4]  = q(i  ,j+1,0);
						waterIndex[waterIndexSize+5]  = q(i  ,j  ,0);
						
						waterIndex[waterIndexSize+6]  = q(i  ,j  ,1);
						waterIndex[waterIndexSize+7]  = q(i+1,j+1,0);
						waterIndex[waterIndexSize+8]  = q(i  ,j+1,0);
						
						waterIndex[waterIndexSize+9]  = q(i  ,j  ,1);
						waterIndex[waterIndexSize+10] = q(i+1,j  ,0);
						waterIndex[waterIndexSize+11] = q(i+1,j+1,0);
						waterIndexSize += 12;
					}
				}
			}
			sectors[x*ySectors + y].geometryIndexSize = geometryIndexSize - sectors[x*ySectors + y].geometryIndexOffset;
			sectors[x*ySectors + y].waterIndexSize = waterIndexSize - sectors[x*ySectors + y].waterIndexOffset;
		}
	}
	glGenBuffers(1, &geometryVBO); glError();
	glBindBuffer(GL_ARRAY_BUFFER, geometryVBO); glError();
	glBufferData(GL_ARRAY_BUFFER, sizeof(RenderVertex)*geometrySize, geometry, GL_DYNAMIC_DRAW); glError();
	free(geometry);
	
	glGenBuffers(1, &geometryIndexVBO); glError();
	glBindBuffer(GL_ARRAY_BUFFER, geometryIndexVBO); glError();
	glBufferData(GL_ARRAY_BUFFER, sizeof(GLuint)*geometryIndexSize, geometryIndex, GL_STATIC_DRAW); glError();
	free(geometryIndex);
	
	glGenBuffers(1, &waterVBO); glError();
	glBindBuffer(GL_ARRAY_BUFFER, waterVBO); glError();
	glBufferData(GL_ARRAY_BUFFER, sizeof(RenderVertex)*waterSize, water, GL_DYNAMIC_DRAW); glError();
	free(water);
	
	glGenBuffers(1, &waterIndexVBO); glError();
	glBindBuffer(GL_ARRAY_BUFFER, waterIndexVBO); glError();
	glBufferData(GL_ARRAY_BUFFER, sizeof(GLuint)*waterIndexSize, waterIndex, GL_STATIC_DRAW); glError();
	free(waterIndex);

	
	////////////////////
	// fill the texture part of the sectors
	texture = malloc(sizeof(PIELIGHT)*xSectors*ySectors*(sectorSize+1)*(sectorSize+1)*2*numGroundTypes);
	textureIndex = malloc(sizeof(GLuint)*xSectors*ySectors*sectorSize*sectorSize*12*numGroundTypes);
	textureSize = 0;
	textureIndexSize = 0;
	for (layer = 0; layer < numGroundTypes; layer++)
	{
		for (x = 0; x < xSectors; x++)
		{
			for (y = 0; y < ySectors; y++)
			{
				if (layer == 0)
				{
					sectors[x*ySectors + y].textureOffset = malloc(sizeof(int)*numGroundTypes);
					sectors[x*ySectors + y].textureSize = malloc(sizeof(int)*numGroundTypes);
					sectors[x*ySectors + y].textureIndexOffset = malloc(sizeof(int)*numGroundTypes);
					sectors[x*ySectors + y].textureIndexSize = malloc(sizeof(int)*numGroundTypes);
				}

				sectors[x*ySectors + y].textureOffset[layer] = textureSize;
				sectors[x*ySectors + y].textureSize[layer] = 0;
				sectors[x*ySectors + y].textureIndexOffset[layer] = textureIndexSize;
				sectors[x*ySectors + y].textureIndexSize[layer] = 0;
				//debug(LOG_WARNING, "offset when filling %i: %i", layer, xSectors*ySectors*(sectorSize+1)*(sectorSize+1)*2*layer);
				for (i = 0; i < sectorSize+1; i++)
				{
					for (j = 0; j < sectorSize+1; j++)
					{
						bool draw = false;
						bool off_map;

						// set transparency
						for (a=0;a<2;a++)
						{
							for(b=0;b<2;b++)
							{
								absX = x*sectorSize+i+a;
								absY = y*sectorSize+j+b;
								colour[a][b].rgba = 0x00FFFFFF; // transparent
								
								// extend the terrain type for the bottom and left edges of the map
								off_map = false;
								if (absX == mapWidth)
								{
									off_map = true;
									absX--;
								}
								if (absY == mapHeight)
								{
									off_map = true;
									absY--;
								}
								
								if (absX < 0 || absY < 0 || absX >= mapWidth || absY >= mapHeight)
								{
									// not on the map, so don't draw
									continue;
								}
								if (mapTile(absX,absY)->ground == layer)
								{
									colour[a][b].rgba = 0xFFFFFFFF;
									if (!off_map)
									{
										// if this point lies on the edge is may not force this tile to be drawn
										// otherwise this will give a bright line when fog is enabled
										draw = true;
									}
								}
							}
						}
						texture[xSectors*ySectors*(sectorSize+1)*(sectorSize+1)*2*layer+((x*ySectors+y)*(sectorSize+1)*(sectorSize+1)*2 + (i*(sectorSize+1)+j)*2)].rgba = colour[0][0].rgba;
						averageColour(&centerColour, colour[0][0], colour[0][1], colour[1][0], colour[1][1]);
						texture[xSectors*ySectors*(sectorSize+1)*(sectorSize+1)*2*layer+((x*ySectors+y)*(sectorSize+1)*(sectorSize+1)*2 + (i*(sectorSize+1)+j)*2+1)].rgba = centerColour.rgba;
						textureSize += 2;
						if ((draw) && i < sectorSize && j < sectorSize)
						{
							textureIndex[textureIndexSize+0]  = q(i  ,j  ,1);
							textureIndex[textureIndexSize+1]  = q(i  ,j  ,0);
							textureIndex[textureIndexSize+2]  = q(i+1,j  ,0);
							
							textureIndex[textureIndexSize+3]  = q(i  ,j  ,1);
							textureIndex[textureIndexSize+4]  = q(i  ,j+1,0);
							textureIndex[textureIndexSize+5]  = q(i  ,j  ,0);
							
							textureIndex[textureIndexSize+6]  = q(i  ,j  ,1);
							textureIndex[textureIndexSize+7]  = q(i+1,j+1,0);
							textureIndex[textureIndexSize+8]  = q(i  ,j+1,0);
							
							textureIndex[textureIndexSize+9]  = q(i  ,j  ,1);
							textureIndex[textureIndexSize+10] = q(i+1,j  ,0);
							textureIndex[textureIndexSize+11] = q(i+1,j+1,0);
							textureIndexSize += 12;
						}

					}
				}
				sectors[x*ySectors + y].textureSize[layer] = textureSize - sectors[x*ySectors + y].textureOffset[layer];
				sectors[x*ySectors + y].textureIndexSize[layer] = textureIndexSize - sectors[x*ySectors + y].textureIndexOffset[layer];
			}
		}
	}
	glGenBuffers(1, &textureVBO); glError();
	glBindBuffer(GL_ARRAY_BUFFER, textureVBO); glError();
	glBufferData(GL_ARRAY_BUFFER, sizeof(PIELIGHT)*xSectors*ySectors*(sectorSize+1)*(sectorSize+1)*2*numGroundTypes, texture, GL_STATIC_DRAW); glError();
	free(texture);
	
	glGenBuffers(1, &textureIndexVBO); glError();
	glBindBuffer(GL_ARRAY_BUFFER, textureIndexVBO); glError();
	glBufferData(GL_ARRAY_BUFFER, sizeof(GLuint)*textureIndexSize, textureIndex, GL_STATIC_DRAW); glError();
	free(textureIndex);

	// and finally the decals
	decaldata = malloc(sizeof(DecalVertex)*mapWidth*mapHeight*12);
	decalSize = 0;
	for (x = 0; x < xSectors; x++)
	{
		for (y = 0; y < ySectors; y++)
		{
			sectors[x*ySectors + y].decalOffset = decalSize;
			sectors[x*ySectors + y].decalSize = 0;
			setSectorDecals(x, y, decaldata, &decalSize);
			sectors[x*ySectors + y].decalSize = decalSize - sectors[x*ySectors + y].decalOffset;
		}
	}
	debug(LOG_TERRAIN, "%i decals found", decalSize/12);
	glGenBuffers(1, &decalVBO); glError();
	glBindBuffer(GL_ARRAY_BUFFER, decalVBO); glError();
	glBufferData(GL_ARRAY_BUFFER, sizeof(DecalVertex)*decalSize, decaldata, GL_STATIC_DRAW); glError();
	free(decaldata);
	
	lightmap_tex_num = 0;
	lightmapNextUpdate = 0;
	// determine the smallest power-of-two size we can use for the lightmap
	size = mapWidth;
	if (mapHeight > size)
	{
		size = mapHeight;
	}
	lightmapSize = 1;
	while (size) 
	{
		size >>= 1;
		lightmapSize <<=1;
	}
	debug(LOG_TERRAIN, "the size of the map is %ix%i", mapWidth, mapHeight);
	debug(LOG_TERRAIN, "lightmap texture size is %ix%i", lightmapSize, lightmapSize);
	
	terrainInitalised = true;
	
	return true;
}

/// free all memory and opengl buffers used by the terrain renderer
void shutdownTerrain(void)
{
	int x,y;
	glDeleteBuffers(1, &geometryVBO);
	glDeleteBuffers(1, &geometryIndexVBO);
	glDeleteBuffers(1, &waterVBO);
	glDeleteBuffers(1, &waterIndexVBO);
	glDeleteBuffers(1, &textureVBO);
	glDeleteBuffers(1, &textureIndexVBO);
	glDeleteBuffers(1, &decalVBO);
	
	for (x = 0; x < xSectors; x++)
	{
		for (y = 0; y < ySectors; y++)
		{
			free(sectors[x*ySectors + y].textureOffset);
			free(sectors[x*ySectors + y].textureSize);
			free(sectors[x*ySectors + y].textureIndexOffset);
			free(sectors[x*ySectors + y].textureIndexSize);
		}
	}
	free(sectors);
	
	terrainInitalised = false;
}

void drawTerrain(void)
{
	int i, j, x, y;
	int texPage;
	PIELIGHT colour;
	int layer;
	int offset, size;
	float xPos, yPos, distance;
	GLfloat paramsX[4] = {0, 0, -1.0/world_coord(mapHeight)*((float)mapHeight/lightmapSize), 0}; 
	GLfloat paramsY[4] = {1.0/world_coord(mapWidth)*((float)mapWidth/lightmapSize), 0, 0, 0};	

	glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_TEXTURE_BIT | GL_DEPTH_BUFFER_BIT);
	
	// lightmap
	// we limit the framerate of the lightmap, because uploading a texture is an expensive operation
	if (gameTime > lightmapNextUpdate)
	{
		// allocate 3D array for our lightmap--but MSVC don't support VLA's :P
		unsigned char *data=(unsigned char * )malloc(lightmapSize * lightmapSize * 3 * sizeof(unsigned char));
		
		lightmapNextUpdate = gameTime + 80;
		glDeleteTextures(1, &lightmap_tex_num);

		glGenTextures(1, &lightmap_tex_num);
		
		for (i = 0; i < lightmapSize; i++)
		{
			for (j = 0; j < lightmapSize; j++)
			{
				float darken, distToEdge;
				float distA, distB, distC, distD;
				float playerX = (float)player.p.x/TILE_UNITS+visibleTiles.x/2;
				float playerY = (float)player.p.z/TILE_UNITS+visibleTiles.y/2;
				getColour(&colour, i, j, false);
				data[(i * lightmapSize + j) * 3 + 0] = colour.byte.r;
				data[(i * lightmapSize + j) * 3 + 1] = colour.byte.g;
				data[(i * lightmapSize + j) * 3 + 2] = colour.byte.b;
				
				if (!rendStates.fogEnabled)
				{
					// fade to black at the edges of the visible terrain area
					distA = visibleTiles.x/2-(i-playerX);
					distB = (i-playerX)+visibleTiles.x/2;
					distC = visibleTiles.y/2-(j-playerY);
					distD = (j-playerY)+visibleTiles.y/2;
					
					// calculate the distance to the closest edge of the visible map
					// determine the smallest distance
					distToEdge = distA;
					if (distB < distToEdge) distToEdge = distB;
					if (distC < distToEdge) distToEdge = distC;
					if (distD < distToEdge) distToEdge = distD;

					darken = (distToEdge)/2.0f;
					if (darken < 0)
					{
						darken = 0;
					}
					if (darken < 1)
					{
						data[(i * lightmapSize + j) * 3 + 0] *= darken;
						data[(i * lightmapSize + j) * 3 + 1] *= darken;
						data[(i * lightmapSize + j) * 3 + 2] *= darken;
					}
				}
			}
		}
		
		glBindTexture(GL_TEXTURE_2D, lightmap_tex_num);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexImage2D(GL_TEXTURE_2D, 0, 3, lightmapSize, lightmapSize, 0, GL_RGB, GL_UNSIGNED_BYTE, data);

		// once lightmap has been uploaded, we free it.
		free(data);
	}
	
	///////////////////////////////////
	// now set up the lightmap texture
	
	glActiveTexture(GL_TEXTURE1);
	// load the texture
	glBindTexture(GL_TEXTURE_2D, lightmap_tex_num);
	glEnable(GL_TEXTURE_2D);

	glEnable(GL_TEXTURE_GEN_S); glError();
	glEnable(GL_TEXTURE_GEN_T); glError();
	glEnable(GL_BLEND);
	
	// set the parameters for the texture coord generation
	glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
	glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
	glTexGenfv(GL_S, GL_OBJECT_PLANE, paramsX);
	glTexGenfv(GL_T, GL_OBJECT_PLANE, paramsY);
	
	// shift the lightmap half a tile as lights are supposed to be placed at the center of a tile
	glMatrixMode(GL_TEXTURE);
	glLoadIdentity();
	glTranslatef(1.0/lightmapSize/2, 1.0/lightmapSize/2, 0);
	glMatrixMode(GL_MODELVIEW);
	
	glActiveTexture(GL_TEXTURE0);
	
	///////////////////////////////////
	// terrain culling
	for (x = 0; x < xSectors; x++)
	{
		for (y = 0; y < ySectors; y++)
		{
			xPos = world_coord(x*sectorSize+sectorSize/2);
			yPos = world_coord(y*sectorSize+sectorSize/2);
			distance = pow(player.p.x+world_coord(visibleTiles.x/2) - xPos, 2) + pow(player.p.z+world_coord(visibleTiles.y/2) - yPos, 2);
			//debug(LOG_TERRAIN, "culling: %i,%i player x: %i, player z: %i, xPos: %f, yPos: %f, distance = %f, max = %f",  x,y,player.p.x, player.p.z, xPos, yPos, distance, pow(world_coord(terrainDistance),2));
			if (distance > pow(world_coord(terrainDistance),2))
			{
				sectors[x*ySectors + y].draw = false;
			}
			else
			{
				sectors[x*ySectors + y].draw = true;
				if (sectors[x*ySectors + y].dirty)
				{
					updateSectorGeometry(x,y);
					sectors[x*ySectors + y].dirty = false;
				}
			}
		}
	}


	
	//////////////////////////////////////
	// canvas to draw on
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_BLEND);
	// we only draw in the depth buffer of using fog of war, as the clear color is black then
	if (rendStates.fogEnabled)
	{
		// FIXME: with fog enabled we draw this also to the color buffer. A fogbox might be faster.
		glDisable(GL_FOG);
		glColor4ub(0x00, 0x00, 0x00, 0xFF);
	}
	else
	{
		glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	}
	// draw slightly higher distance than it actually is so it will not
	// by accident obscure the actual terrain
	glEnable(GL_POLYGON_OFFSET_FILL);
	glPolygonOffset(0.1f, 1.0f);
	
	// bind the vertex buffer
	glEnableClientState( GL_VERTEX_ARRAY );
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometryIndexVBO); glError();
	glBindBuffer(GL_ARRAY_BUFFER, geometryVBO); glError();

	glVertexPointer(3, GL_FLOAT, sizeof(RenderVertex), BUFFER_OFFSET(0)); glError();
	
	for (x = 0; x < xSectors; x++)
	{
		for (y = 0; y < ySectors; y++)
		{
			if (sectors[x*ySectors + y].draw)
			{
				addDrawRangeElements(GL_TRIANGLES,
				                     sectors[x*ySectors + y].geometryOffset,
				                     sectors[x*ySectors + y].geometryOffset+sectors[x*ySectors + y].geometrySize,
				                     sectors[x*ySectors + y].geometryIndexSize,
				                     GL_UNSIGNED_INT,
				                     sectors[x*ySectors + y].geometryIndexOffset);
			}
		}
	}
	finishDrawRangeElements();
	
	if (rendStates.fogEnabled)
	{
		glEnable(GL_FOG);
		glColor4ub(0xFF, 0xFF, 0xFF, 0xFF);	
	}
	else
	{
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	}

	// disable the depth offset
	glDisable(GL_POLYGON_OFFSET_FILL);
	
	///////////////////////////////////
	// terrain
	// we are going to use this one
	glEnable(GL_TEXTURE_2D);
	glEnableClientState( GL_COLOR_ARRAY );
	
	// set up for texture coord generation
	glEnable(GL_TEXTURE_GEN_S);
	glEnable(GL_TEXTURE_GEN_T);
	glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
	glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);

	// additive blending
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE);
	// only draw colors
	glDepthMask(GL_FALSE);
	
	glEnableClientState( GL_VERTEX_ARRAY );
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, textureIndexVBO); glError();
	glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
	
	// load the vertex (geometry) buffer
	glBindBuffer(GL_ARRAY_BUFFER, geometryVBO);
	glVertexPointer(3, GL_FLOAT, sizeof(RenderVertex), BUFFER_OFFSET(0)); glError();

	glBindBuffer(GL_ARRAY_BUFFER, textureVBO);

	// draw each layer separately
	for (layer = 0; layer < numGroundTypes; layer++)
	{
		GLfloat paramsX[4] = {0, 0, -1.0/world_coord(psGroundTypes[layer].textureSize), 0}; 
		GLfloat paramsY[4] = {1.0/world_coord(psGroundTypes[layer].textureSize), 0, 0, 0};
		// load the texture
		texPage = iV_GetTexture(psGroundTypes[layer].textureName);
		pie_SetTexturePage(texPage);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
		
		// set the texture generation parameters
		glTexGenfv(GL_S, GL_OBJECT_PLANE, paramsX);
		glTexGenfv(GL_T, GL_OBJECT_PLANE, paramsY);
		
		// load the color buffer
		glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(PIELIGHT), BUFFER_OFFSET(sizeof(PIELIGHT)*xSectors*ySectors*(sectorSize+1)*(sectorSize+1)*2*layer));
		
		for (x = 0; x < xSectors; x++)
		{
			for (y = 0; y < ySectors; y++)
			{
				if (sectors[x*ySectors + y].draw)
				{
					addDrawRangeElements(GL_TRIANGLES,
										 sectors[x*ySectors + y].geometryOffset,
										 sectors[x*ySectors + y].geometryOffset+sectors[x*ySectors + y].geometrySize,
										 sectors[x*ySectors + y].textureIndexSize[layer],
										 GL_UNSIGNED_INT,
										 sectors[x*ySectors + y].textureIndexOffset[layer]);
				}
			}
		}
		finishDrawRangeElements();
	}
	// we don't need this one anymore
	glDisableClientState( GL_COLOR_ARRAY );
	//////////////////////////////////
	// decals
	
	// no texture generation
	glDisable(GL_TEXTURE_GEN_S); glError();
	glDisable(GL_TEXTURE_GEN_T); glError();
	
	// select the terrain texture page
	pie_SetTexturePage(terrainPage); glError();
	// use the alpha to blend
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	// and the texture coordinates buffer
	glEnableClientState( GL_TEXTURE_COORD_ARRAY ); glError();
	glEnableClientState( GL_VERTEX_ARRAY ); glError();
	glBindBuffer(GL_ARRAY_BUFFER, decalVBO); glError();
	glVertexPointer(3, GL_FLOAT, sizeof(DecalVertex), BUFFER_OFFSET(0)); glError();
	glTexCoordPointer(2, GL_FLOAT, sizeof(DecalVertex), BUFFER_OFFSET(12)); glError();

	size = 0;
	offset = 0;
	for (x = 0; x < xSectors; x++)
	{
		for (y = 0; y < ySectors + 1; y++)
		{
			if (y < ySectors && offset + size == sectors[x*ySectors + y].decalOffset && sectors[x*ySectors + y].draw)
			{
				// append
				size += sectors[x*ySectors + y].decalSize;
				continue;
			}
			// can't append, so draw what we have and start anew
			if (size > 0)
			{
				glDrawArrays(GL_TRIANGLES, offset, size); glError();
			}
			size = 0;
			if (y < ySectors && sectors[x*ySectors + y].draw)
			{
				offset = sectors[x*ySectors + y].decalOffset;
				size = sectors[x*ySectors + y].decalSize;
			}
		}
	}

	////////////////////////////////
	// disable the lightmap texture
	glActiveTexture(GL_TEXTURE1);
	glDisable(GL_TEXTURE_2D);
	glActiveTexture(GL_TEXTURE0);

	// leave everything in a sane state so it won't mess up somewhere else
	glDisableClientState( GL_TEXTURE_COORD_ARRAY );
	glDisable(GL_TEXTURE_GEN_S);
	glDisable(GL_TEXTURE_GEN_T);
	
	glDisableClientState( GL_VERTEX_ARRAY );
	
	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
	
	glPopAttrib();
}

void drawWater(void)
{
	float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	int x,y;
	const GLfloat paramsX[4] = {0, 0, -1.0/world_coord(4), 0}; 
	const GLfloat paramsY[4] = {1.0/world_coord(4), 0, 0, 0};
	const GLfloat paramsX2[4] = {0, 0, -1.0/world_coord(5), 0}; 
	const GLfloat paramsY2[4] = {1.0/world_coord(5), 0, 0, 0};
	
	glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_TEXTURE_BIT | GL_DEPTH_BUFFER_BIT | GL_FOG_BIT);

	glEnable(GL_TEXTURE_GEN_S); glError();
	glEnable(GL_TEXTURE_GEN_T); glError();
	
	glColor4ub(0xFF, 0xFF, 0xFF, 0xFF);
	glDepthMask(GL_FALSE);
	
	if (rendStates.fogEnabled)
	{
		glFogfv(GL_FOG_COLOR, white);
	}
	
	// first texture unit
	pie_SetTexturePage(iV_GetTexture("page-80")); glError();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
	glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
	glTexGenfv(GL_S, GL_OBJECT_PLANE, paramsX);
	glTexGenfv(GL_T, GL_OBJECT_PLANE, paramsY);
	glMatrixMode(GL_TEXTURE);
	glLoadIdentity();
	glTranslatef(waterOffset, 0, 0);

	// multiplicative blending
	glEnable(GL_BLEND);
	glBlendFunc(GL_ZERO, GL_SRC_COLOR);
	
	// second texture unit
	glActiveTexture(GL_TEXTURE1);
	pie_SetTexturePage(iV_GetTexture("page-81")); glError();
	glEnable(GL_TEXTURE_2D);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glEnable(GL_TEXTURE_GEN_S); glError();
	glEnable(GL_TEXTURE_GEN_T); glError();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
	glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
	glTexGenfv(GL_S, GL_OBJECT_PLANE, paramsX2);
	glTexGenfv(GL_T, GL_OBJECT_PLANE, paramsY2);
	glLoadIdentity();
	glTranslatef(-waterOffset, 0, 0);

	// bind the vertex buffer
	glEnableClientState( GL_VERTEX_ARRAY );
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, waterIndexVBO); glError();
	glBindBuffer(GL_ARRAY_BUFFER, waterVBO); glError();

	glVertexPointer(3, GL_FLOAT, sizeof(RenderVertex), BUFFER_OFFSET(0)); glError();

	for (x = 0; x < xSectors; x++)
	{
		for (y = 0; y < ySectors; y++)
		{
			if (sectors[x*ySectors + y].draw)
			{
				addDrawRangeElements(GL_TRIANGLES,
				                     sectors[x*ySectors + y].geometryOffset,
				                     sectors[x*ySectors + y].geometryOffset+sectors[x*ySectors + y].geometrySize,
				                     sectors[x*ySectors + y].waterIndexSize,
				                     GL_UNSIGNED_INT,
				                     sectors[x*ySectors + y].waterIndexOffset);
			}
		}
	}
	finishDrawRangeElements();
	
	glDisableClientState( GL_VERTEX_ARRAY );
	
	// move the water
	if(!gamePaused())
	{
		waterOffset += timeAdjustedIncrement(0.1f, true);
	}

	// disable second texture
	glLoadIdentity();
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_TEXTURE_GEN_S);
	glDisable(GL_TEXTURE_GEN_T);
	glActiveTexture(GL_TEXTURE0);

	// clean up
	glDisable(GL_TEXTURE_GEN_S);
	glDisable(GL_TEXTURE_GEN_T);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glDepthMask(GL_TRUE);
	glDisable(GL_TEXTURE_GEN_S);
	glDisable(GL_TEXTURE_GEN_T);
	
	glPopAttrib();
}
