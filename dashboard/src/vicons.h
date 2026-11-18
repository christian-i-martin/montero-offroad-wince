/* Pictograms for the map's place markers (vicons.c). The numbers are stored in the vector map file
 * (tools/vmap/build_vmap.py ICONS), so keep them in this order. Portable C. */
#ifndef VICONS_H
#define VICONS_H

enum {
    VICON_FUEL,
    VICON_PARKING,
    VICON_MALL,
    VICON_HOSPITAL,
    VICON_POLICE,
    VICON_PHARMACY,
    VICON_BANK,
    VICON_SUPERMARKET,
    VICON_CONVENIENCE,
    VICON_FOOD,
    VICON_CAR_REPAIR,
    VICON_HOTEL,
    VICON_CHURCH,
    VICON_SCHOOL,
    VICON_TOWN_HALL,
    VICON_BUS,
    VICON_TOLL,
    VICON_FIRE,
    VICON_POST,
    VICON_TOILETS,
    VICON_CAFE,
    VICON_BAKERY,
    VICON_HARDWARE,
    VICON_MOTORCYCLE,
    VICON_DENTIST,
    VICON_ATTRACTION,
    VICON_CAMPSITE,
    VICON_WATER,
    VICON_COUNT
};

#define VICON_SIZE 16   /* pictograms are 16 x 16 pixels */

/* The pictogram: 16 rows of 16 characters, '#' for a lit pixel; NULL for an unknown number. */
const char *vicon_bitmap(int icon);

#endif
