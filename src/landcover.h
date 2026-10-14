#ifndef LANDCOVER_H
#define LANDCOVER_H
typedef enum {
    COVER_UNKNOWN = 0,
    COVER_TREE = 10,
    COVER_SHRUB = 20,
    COVER_GRASS = 30,
    COVER_CROP = 40,
    COVER_BUILT = 50,
    COVER_BARE = 60,
    COVER_SNOW = 70,
    COVER_WATER = 80,
    COVER_WETLAND = 90,
    COVER_MANGROVE = 95,
    COVER_MOSS = 100
} LandCover;
#endif
