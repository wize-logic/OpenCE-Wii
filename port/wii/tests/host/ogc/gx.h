/* host stand-in for libogc's <ogc/gx.h>: the constants gx_psh.c names, with libogc's values,
   and its TEV calls as nothing (tests/gx_psh_check.c) */
#define GX_FALSE			0
#define GX_TRUE				1
#define GX_COLOR0A0			4
#define GX_COLOR1A1			5
#define GX_COLORNULL		0xff
#define GX_TEXCOORD0		0x0
#define GX_TEXCOORDNULL		0xff
#define GX_CC_CPREV			0				/*!< Use the color value from previous TEV stage */
#define GX_CC_APREV			1				/*!< Use the alpha value from previous TEV stage */
#define GX_CC_C0			2				/*!< Use the color value from the color/output register 0 */
#define GX_CC_A0			3				/*!< Use the alpha value from the color/output register 0 */
#define GX_CC_C1			4				/*!< Use the color value from the color/output register 1 */
#define GX_CC_A1			5				/*!< Use the alpha value from the color/output register 1 */
#define GX_CC_C2			6				/*!< Use the color value from the color/output register 2 */
#define GX_CC_A2			7				/*!< Use the alpha value from the color/output register 2 */
#define GX_CC_TEXC			8				/*!< Use the color value from texture */
#define GX_CC_TEXA			9				/*!< Use the alpha value from texture */
#define GX_CC_RASC			10				/*!< Use the color value from rasterizer */
#define GX_CC_RASA			11				/*!< Use the alpha value from rasterizer */
#define GX_CC_ONE			12
#define GX_CC_HALF			13
#define GX_CC_KONST			14
#define GX_CC_ZERO			15				/*!< Use to pass zero value */
#define GX_CA_APREV			0				/*!< Use the alpha value from previous TEV stage */
#define GX_CA_A0			1				/*!< Use the alpha value from the color/output register 0 */
#define GX_CA_A1			2				/*!< Use the alpha value from the color/output register 1 */
#define GX_CA_A2			3				/*!< Use the alpha value from the color/output register 2 */
#define GX_CA_TEXA			4				/*!< Use the alpha value from texture */
#define GX_CA_RASA			5				/*!< Use the alpha value from rasterizer */
#define GX_CA_KONST			6
#define GX_CA_ZERO			7				/*!< Use to pass zero value */
#define GX_TEVSTAGE0		0
#define GX_TEV_ADD				0
#define GX_TEV_SUB				1
#define GX_TB_ZERO				0
#define GX_TB_ADDHALF			1
#define GX_TB_SUBHALF			2
#define GX_CS_SCALE_1			0
#define GX_CS_SCALE_2			1
#define GX_CS_SCALE_4			2
#define GX_CS_DIVIDE_2			3
#define GX_TEVPREV				0			/*!< Default register for passing results from one stage to another. */
#define GX_TEVREG0				1
#define GX_TEVREG1				2
#define GX_TEVREG2				3
#define GX_TEXMAP0				0			/*!< Texture map slot 0 */
#define GX_TEXMAP_NULL			0xff			/*!< No texmap */
#define GX_KCOLOR0				0			/*!< Constant register 0 */
#define GX_TEV_KCSEL_1					0x00			/*!< constant 1.0 */
#define GX_TEV_KCSEL_7_8				0x01			/*!< constant 7/8 */
#define GX_TEV_KCSEL_3_4				0x02			/*!< constant 3/4 */
#define GX_TEV_KCSEL_5_8				0x03			/*!< constant 5/8 */
#define GX_TEV_KCSEL_1_2				0x04			/*!< constant 1/2 */
#define GX_TEV_KCSEL_3_8				0x05			/*!< constant 3/8 */
#define GX_TEV_KCSEL_1_4				0x06			/*!< constant 1/4 */
#define GX_TEV_KCSEL_1_8				0x07			/*!< constant 1/8 */
#define GX_TEV_KCSEL_K0					0x0C			/*!< K0[RGB] register */
#define GX_TEV_KCSEL_K1					0x0D			/*!< K1[RGB] register */
#define GX_TEV_KCSEL_K2					0x0E			/*!< K2[RGB] register */
#define GX_TEV_KCSEL_K3					0x0F			/*!< K3[RGB] register */
#define GX_TEV_KCSEL_K0_R				0x10			/*!< K0[RRR] register */
#define GX_TEV_KCSEL_K1_R				0x11			/*!< K1[RRR] register */
#define GX_TEV_KCSEL_K2_R				0x12			/*!< K2[RRR] register */
#define GX_TEV_KCSEL_K3_R				0x13			/*!< K3[RRR] register */
#define GX_TEV_KCSEL_K0_G				0x14			/*!< K0[GGG] register */
#define GX_TEV_KCSEL_K1_G				0x15			/*!< K1[GGG] register */
#define GX_TEV_KCSEL_K2_G				0x16			/*!< K2[GGG] register */
#define GX_TEV_KCSEL_K3_G				0x17			/*!< K3[GGG] register */
#define GX_TEV_KCSEL_K0_B				0x18			/*!< K0[BBB] register */
#define GX_TEV_KCSEL_K1_B				0x19			/*!< K1[BBB] register */
#define GX_TEV_KCSEL_K2_B				0x1A			/*!< K2[BBB] register */
#define GX_TEV_KCSEL_K3_B				0x1B			/*!< K3[RBB] register */
#define GX_TEV_KCSEL_K0_A				0x1C			/*!< K0[AAA] register */
#define GX_TEV_KCSEL_K1_A				0x1D			/*!< K1[AAA] register */
#define GX_TEV_KCSEL_K2_A				0x1E			/*!< K2[AAA] register */
#define GX_TEV_KCSEL_K3_A				0x1F			/*!< K3[AAA] register */
#define GX_TEV_KASEL_1					0x00			/*!< constant 1.0 */
#define GX_TEV_KASEL_7_8				0x01			/*!< constant 7/8 */
#define GX_TEV_KASEL_3_4				0x02			/*!< constant 3/4 */
#define GX_TEV_KASEL_5_8				0x03			/*!< constant 5/8 */
#define GX_TEV_KASEL_1_2				0x04			/*!< constant 1/2 */
#define GX_TEV_KASEL_3_8				0x05			/*!< constant 3/8 */
#define GX_TEV_KASEL_1_4				0x06			/*!< constant 1/4 */
#define GX_TEV_KASEL_1_8				0x07			/*!< constant 1/8 */
#define GX_TEV_KASEL_K0_R				0x10			/*!< K0[R] register */
#define GX_TEV_KASEL_K1_R				0x11			/*!< K1[R] register */
#define GX_TEV_KASEL_K2_R				0x12			/*!< K2[R] register */
#define GX_TEV_KASEL_K3_R				0x13			/*!< K3[R] register */
#define GX_TEV_KASEL_K0_G				0x14			/*!< K0[G] register */
#define GX_TEV_KASEL_K1_G				0x15			/*!< K1[G] register */
#define GX_TEV_KASEL_K2_G				0x16			/*!< K2[G] register */
#define GX_TEV_KASEL_K3_G				0x17			/*!< K3[G] register */
#define GX_TEV_KASEL_K0_B				0x18			/*!< K0[B] register */
#define GX_TEV_KASEL_K1_B				0x19			/*!< K1[B] register */
#define GX_TEV_KASEL_K2_B				0x1A			/*!< K2[B] register */
#define GX_TEV_KASEL_K3_B				0x1B			/*!< K3[B] register */
#define GX_TEV_KASEL_K0_A				0x1C			/*!< K0[A] register */
#define GX_TEV_KASEL_K1_A				0x1D			/*!< K1[A] register */
#define GX_TEV_KASEL_K2_A				0x1E			/*!< K2[A] register */
#define GX_TEV_KASEL_K3_A				0x1F			/*!< K3[A] register */
#define GX_TEV_SWAP0					0
#define GX_TEV_SWAP1					1
#define GX_TEV_SWAP2					2
#define GX_TEV_SWAP3					3
#define GX_CH_RED						0
#define GX_CH_GREEN						1
#define GX_CH_BLUE						2
#define GX_CH_ALPHA						3
typedef unsigned char u8;
typedef struct { u8 r, g, b, a; } GXColor;
typedef float f32;
#define GCBOOL int
static inline void GX_SetTevKColor(u8 a, GXColor c) { (void)a; (void)c; }
static inline void GX_SetNumTevStages(u8 n) { (void)n; }
static inline void GX_SetTevOrder(u8 a, u8 b, u8 c, u8 d) { (void)a; (void)b; (void)c; (void)d; }
static inline void GX_SetTevSwapMode(u8 a, u8 b, u8 c) { (void)a; (void)b; (void)c; }
static inline void GX_SetTevKColorSel(u8 a, u8 b) { (void)a; (void)b; }
static inline void GX_SetTevKAlphaSel(u8 a, u8 b) { (void)a; (void)b; }
static inline void GX_SetTevColorIn(u8 s, u8 a, u8 b, u8 c, u8 d) { (void)s; (void)a; (void)b; (void)c; (void)d; }
static inline void GX_SetTevAlphaIn(u8 s, u8 a, u8 b, u8 c, u8 d) { (void)s; (void)a; (void)b; (void)c; (void)d; }
static inline void GX_SetTevColorOp(u8 s, u8 a, u8 b, u8 c, u8 d, u8 e) { (void)s; (void)a; (void)b; (void)c; (void)d; (void)e; }
static inline void GX_SetTevAlphaOp(u8 s, u8 a, u8 b, u8 c, u8 d, u8 e) { (void)s; (void)a; (void)b; (void)c; (void)d; (void)e; }
static inline void GX_SetTevSwapModeTable(u8 a, u8 b, u8 c, u8 d, u8 e) { (void)a; (void)b; (void)c; (void)d; (void)e; }
