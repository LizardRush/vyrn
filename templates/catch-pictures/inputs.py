# inputs.py
import vyrn

def see(world):
    picture = vyrn.canvas(10, 10)
    picture.box(world.basket - 1, 9, 3, 1, "gray")
    picture.dot(world.fruit_x, world.fruit_y, "white")
    return picture
