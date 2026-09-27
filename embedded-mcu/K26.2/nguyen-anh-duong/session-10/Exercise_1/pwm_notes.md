# PWM notes

## 1. Why the LED doesn't look linear

I turned the knob slowly from one end to the other and most of the visible change
is at the very start of the travel. The first bit of turning already takes the LED
from off to something that looks quite bright, and then the last half of the knob
barely seems to do anything - it just looks "on" the whole time. The duty cycle
is going up in even steps (I checked the raw/duty in the log), so it's the eye
that isn't linear, not the PWM.

I think the reason is that our eyes respond to light roughly logarithmically.
What looks like an even change in brightness is an even *ratio*, not an even
difference. Going from 0% to 10% duty is a huge relative jump (basically infinite,
from nothing to something), 10% to 20% doubles the light, but 90% to 100% is only
about a 10% increase, which is hard to notice. So all the drama is at the bottom
end of the knob and the top end looks flat.

If you wanted to fix it you'd apply some gamma correction, e.g.
duty = max * (raw/max)^2.2, but the exercise only asks to describe it so I left
the mapping linear.

## 2. What limits frequency and resolution

The LEDC timer doesn't make the PWM straight from the CPU clock, it divides a
clock down. For a resolution of n bits it needs 2^n steps per period, so:

    clock needed = freq * 2^resolution

For my setup, 5 kHz at 13 bits:

    5000 * 8192 = 40,960,000 Hz = about 41 MHz

The LEDC clock source is 80 MHz APB so 41 MHz is fine, and it configures OK.

That also shows why the two aren't independent - for a fixed clock, if you push
the resolution up you have to drop the frequency, and the other way round.

If I ask for 13 bits at 5 MHz it would need 5,000,000 * 8192 = about 41 GHz,
which obviously doesn't exist. When I tried it, ledc_timer_config() refused it
and returned:

    E (...) ledc: requested frequency and duty resolution can not be achieved,
              try reducing the duty resolution or increasing the clock
    ESP_ERROR_CHECK failed: esp_err_t 0x102 (ESP_ERR_INVALID_ARG)

So the combination has to keep freq * 2^resolution under what the clock can
actually divide to.

Values I used:

- LEDC_FREQ = 5000 Hz. Well above the flicker range so nothing visible, and low
  enough to still allow 13 bits.
- LEDC_RES = 13 bit, so duty goes 0..8191. That's fine enough that I don't see
  any stepping at either end.

