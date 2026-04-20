/* ==============================================================
 * Cinux Big Kernel - Embedded PSF2 Font Data
 * ==============================================================

 * Uses GAS .incbin to embed the PSF2 font binary directly into
 * the kernel ELF with clean, predictable symbol names.
 *
 * Symbols exported:
 *   font_psf_start  -- beginning of font data
 *   font_psf_end    -- one past the last byte
 *   font_psf_size   -- 4-byte little-endian size value
 *
 * The assembler include path must include the project root so
 * that .incbin can locate assets/font.psf.
 */

.section .rodata

.global font_psf_start
.type   font_psf_start, @object
font_psf_start:
    .incbin "assets/font.psf"

.global font_psf_end
font_psf_end:

.global font_psf_size
.type   font_psf_size, @object
font_psf_size:
    .long font_psf_end - font_psf_start
