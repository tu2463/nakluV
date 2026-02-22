/*
*/
void colr_color(/* convert short to float color */ 
    COLOR col, 
    COLR clr)
{
    double f;
    if (clr[EXP] == 0)
    {
        col[RED] = col[GRN] = col[BLU] = 0.0;
        return;
    }
    // It reconstructs the brightness scale from the exponent, then multiplies each channel by it.
    /*
    from setcolr: 
    clr[RED] = (int)(r * d);
    clr[EXP] = e + COLXS;
    d = frexp(d, &e) * 256.0 / d;
    the "+8" here accounts for the *256 in setcolr, as 2^8 = 256

    Example: encoded 195 represents values in [195,196)
    So using 195 + 0.5 = 195.5 gives a better approximation to the original float before truncation
    */
    f = ldexp(1.0, (int)clr[EXP] - (COLXS + 8));
    col[RED] = (clr[RED] + 0.5) * f;
    col[GRN] = (clr[GRN] + 0.5) * f;
    col[BLU] = (clr[BLU] + 0.5) * f;
}

/*
This function converts floating-point RGB values (double r, g, b) into Radiance’s RGBE (shared-exponent) 4-byte format (COLR), such that:
r ≈ R × 2^(E - bias - 8)
g ≈ G × 2^(E - bias - 8)
b ≈ B × 2^(E - bias - 8)

Example:
Let: r=10,g=5,b=1, Then: d=10

Step 1: frexp(10)
Binary form: 10=0.625 * 2 ** 4
So: m=0.625,e=4

Step 2: Compute scale
scale = (m * 256) / d = (0.625 * 256) / 10 = 160 / 10 = 16
    ​
Step 3: Scale channels
r = 10 * 16 = 160
g = 5 * 16 = 80
b = 1 * 16 = 16
*/
void setcolr(/* assign a short color value */
             COLR clr,
             double r,
             double g,
             double b)
{
    double d;
    int e;

    // same as d = max(r, g, b)
    // RGBE uses one shared exponent, so it must scale relative to the largest component.
    d = r > g ? r : g;
    if (b > d)
        d = b;

    // If everything is basically zero (black): set all value to 0; avoids numerical complications for multiplying/dividing by extremely small floats
    if (d <= 1e-32)
    {
        clr[RED] = clr[GRN] = clr[BLU] = 0;
        clr[EXP] = 0;
        return;
    }

    /*
    d = frexp(d, &e) = m × 2^e, where 0.5 ≤ m < 1
    then becomes a scaling factor so that the largest channel will map into the 0–255 range: scale = (m * 256) / largest_rgb
    later in effect: scaled_value = original_value × scale
    */
    d = frexp(d, &e) * 256.0 / d;

    // 8-bit_mantissa = int(channel * scale), all > 0
    clr[RED] = (r > 0) * (int)(r * d);
    clr[GRN] = (g > 0) * (int)(g * d);
    clr[BLU] = (b > 0) * (int)(b * d);
    clr[EXP] = e + COLXS; // COLXS is a bias constant that make the stored_exponent fit in one unsigned byte (because e might be negative). stored_exponent = real_exponent + bias
}