#ifndef GENECC_H
#define GENECC_H

typedef unsigned char	u8;
typedef unsigned int	u32;
typedef signed int		s32;

#define GENECC_LAYOUT_LEGACY		1
#define GENECC_LAYOUT_DM365_RBL		2

void genecc_init(void);
unsigned char *do_genecc(const u8 *src, int layout);

#endif // GENECC_H
