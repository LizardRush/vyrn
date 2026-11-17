# points.py
import vyrn

def points():
    if game_over():
        return -10                # got hit
    return 1                      # still alive

def game_over():
    r, g, b = vyrn.pixel(20, 20)  # the demo game turns red when you're hit
    return r > 200 and g < 80

def restart():
    vyrn.press("space")
