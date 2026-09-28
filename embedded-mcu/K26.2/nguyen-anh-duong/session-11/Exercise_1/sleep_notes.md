# Sleep Notes

## 1. Which counter reads 5?

rtc_boot_count is the one that got up to 5. ram_boot_count is the one stuck at 1.

It comes down to where the two variables are actually stored. ram_boot_count is a normal global, so it's in regular RAM. Regular RAM lives in the main digital part of the chip, and deep sleep switches that part off. So when the chip wakes up it doesn't carry on from where it stopped, it really does reset and start over. And since it's a real reset, the startup code runs again and puts the globals back to their starting values, so ram_boot_count goes back to 0, then gets incremented once at the top of app_main, so it always ends up showing 1.

rtc_boot_count is declared with RTC_DATA_ATTR, which puts it in RTC slow memory instead of normal RAM. RTC memory sits in the RTC power domain, and that domain stays powered during deep sleep, otherwise the RTC timer couldn't wake the chip at all. So the value never gets lost and never gets reinitialised, it just keeps going up: 1, 2, 3, 4, 5.

You can see both things in the log. The timestamp drops back to near zero every time it wakes from deep sleep, which shows the chip really did reset. But rtc_boot_count keeps climbing while ram_boot_count never moves off 1.

## 2. Why two different functions for the button

For light sleep I had to do two things: gpio_wakeup_enable() to set the pin itself to fire on a LOW level, and esp_sleep_enable_gpio_wakeup() to make the sleep system actually listen to GPIO. I left the second one out the first time and it just never woke on the button, only on the timer.

For deep sleep neither of those works. I had to use esp_sleep_enable_ext0_wakeup() instead.

The reason is what's still powered in each mode. In light sleep the CPU is stopped but most of the digital side is still on, including the normal GPIO peripheral, so that peripheral can still watch the pin for me. In deep sleep that whole digital side is off, GPIO peripheral included, so nothing is left to watch the pin except the RTC hardware (the RTC_IO block), and that's what ext0 uses. That's why the light sleep way can't work from deep sleep, the thing it depends on isn't powered anymore.

## 3. Power use from highest to lowest

I'd say power-on-idle first, then light sleep, then deep sleep.

Idle is the highest because the CPU is running normally, all the clocks are on and the peripherals are clocked. Even when it's not doing anything it's still ticking over. Light sleep is in the middle, the CPU is stopped and its clock is gated which is where most of the saving comes from, but RAM and the digital domain are still powered so it can resume, so there's still a decent amount of current. Deep sleep is the lowest, almost everything is off apart from the RTC domain, just the timer and the RTC memory I'm using for the counter.

The display is the annoying part. Turning the backlight off only kills the LED current, which is the big chunk of it. The display's controller is still powered off the 3.3 V rail though, and that rail stays up even in deep sleep, so it keeps drawing a few mA. That happens in all three states, so my program doesn't really get rid of the display's power at all. To actually do that I'd have to cut the display's VCC with a MOSFET or a load switch, which I didn't do here, I only toggled the backlight pin.

