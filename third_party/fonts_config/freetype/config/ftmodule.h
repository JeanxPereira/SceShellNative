/*
 * FreeType module list of libScePsm.sprx (FW 4.03).
 *
 * Found on the include path before freetype-2.4.11/include, so FreeType's
 * <freetype/config/ftmodule.h> resolves here (docs/CUSTOMIZE, "module list").
 *
 * libScePsm statically links FreeType 2.4.11 (FT_New_Library 0x19a2f0 stores the version
 * 2/4/11). FT_Add_Default_Modules (0x19e8a0) registers, in this order, the 2.4.11
 * default list without the pcf and bdf drivers:
 *   autofitter, truetype, type1, cff, t1cid, pfr, type42, winfonts, psaux, psnames,
 *   pshinter, raster1, sfnt, smooth, smooth-lcd, smooth-lcdv
 * (module names as in libScePsm's string table; the order is the order of the
 * FT_USE_MODULE lines below, which is the order FT_Add_Default_Modules adds them in).
 */

FT_USE_MODULE( FT_Module_Class, autofit_module_class )
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, t1_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, cff_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, t1cid_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, pfr_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, t42_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, winfnt_driver_class )
FT_USE_MODULE( FT_Module_Class, psaux_module_class )
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Module_Class, pshinter_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_raster1_renderer_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_lcd_renderer_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_lcdv_renderer_class )

/* EOF */
