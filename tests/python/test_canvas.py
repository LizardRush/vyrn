"""vyrn.canvas: drawing, clipping and colours (PLAN.md section 1, python-internals.md section 6)."""

import numpy as np
import pytest
from PIL import ImageColor

import vyrn
from vyrn._canvas import Canvas


def rgb(color):
    return np.array(ImageColor.getrgb(color)[:3], dtype=np.uint8)


def test_blank_canvas_is_black():
    canvas = Canvas(4, 3)
    assert (canvas.width, canvas.height) == (4, 3)
    assert canvas.pixels.shape == (3, 4, 3) and canvas.pixels.dtype == np.uint8
    assert not canvas.pixels.any()


@pytest.mark.parametrize("color, expected", [("white", (255, 255, 255)), ("#102030", (16, 32, 48)),
                                             ((1, 2, 3), (1, 2, 3)), ("gray", tuple(rgb("gray"))),
                                             ("red", (255, 0, 0))])
def test_background_colours(color, expected):
    canvas = Canvas(2, 2, color)
    assert (canvas.pixels == np.array(expected, dtype=np.uint8)).all()


def test_dot_uses_x_for_the_column_and_y_for_the_row():
    canvas = Canvas(4, 3)
    canvas.dot(1, 2, "red")
    expected = np.zeros((3, 4, 3), dtype=np.uint8)
    expected[2, 1] = (255, 0, 0)
    np.testing.assert_array_equal(canvas.pixels, expected)


def test_dot_is_white_by_default():
    canvas = Canvas(3, 3)
    canvas.dot(0, 0)
    assert tuple(canvas.pixels[0, 0]) == (255, 255, 255)


def test_box():
    canvas = Canvas(5, 4)
    canvas.box(1, 0, 2, 3, "blue")
    expected = np.zeros((4, 5, 3), dtype=np.uint8)
    expected[0:3, 1:3] = (0, 0, 255)
    np.testing.assert_array_equal(canvas.pixels, expected)
    canvas.box(0, 3, 5, 1)  # white by default
    assert (canvas.pixels[3] == 255).all()


@pytest.mark.parametrize("x, y, w, h, rows, cols", [
    (-1, -1, 3, 3, slice(0, 2), slice(0, 2)),
    (3, 2, 10, 10, slice(2, 3), slice(3, 4)),
    (-5, 1, 20, 1, slice(1, 2), slice(0, 4)),
])
def test_box_is_clipped(x, y, w, h, rows, cols):
    canvas = Canvas(4, 3)
    canvas.box(x, y, w, h, "white")
    expected = np.zeros((3, 4, 3), dtype=np.uint8)
    expected[rows, cols] = 255
    np.testing.assert_array_equal(canvas.pixels, expected)


@pytest.mark.parametrize("x, y", [(-1, 0), (4, 0), (0, 3), (0, -1), (100, 100), (-100, -100)])
def test_dots_outside_are_ignored(x, y):
    canvas = Canvas(4, 3)
    canvas.dot(x, y, "white")
    assert not canvas.pixels.any()


@pytest.mark.parametrize("x, y, w, h", [(10, 0, 2, 2), (0, 10, 2, 2), (-5, -5, 3, 3), (0, 0, 0, 2), (0, 0, 2, 0)])
def test_boxes_outside_or_empty_draw_nothing(x, y, w, h):
    canvas = Canvas(4, 3)
    canvas.box(x, y, w, h, "white")
    assert not canvas.pixels.any()


def test_numpy_conversion():
    canvas = Canvas(3, 2, "#010203")
    array = np.asarray(canvas)
    assert array.shape == (2, 3, 3) and array.dtype == np.uint8
    assert (array == (1, 2, 3)).all()
    floats = np.asarray(canvas, dtype=np.float32)
    assert floats.dtype == np.float32 and (floats == (1.0, 2.0, 3.0)).all()


def test_pixels_is_the_live_array():
    canvas = Canvas(2, 2)
    canvas.pixels[0, 1] = (9, 8, 7)
    assert tuple(np.asarray(canvas)[0, 1]) == (9, 8, 7)
    canvas.dot(1, 1, "white")
    assert tuple(canvas.pixels[1, 1]) == (255, 255, 255)


def test_public_canvas_function():
    canvas = vyrn.canvas(10, 10)
    assert isinstance(canvas, Canvas)
    canvas.box(4, 9, 3, 1, "gray")
    canvas.dot(2, 0, "white")
    assert tuple(canvas.pixels[9, 4]) == tuple(rgb("gray")) and tuple(canvas.pixels[9, 7]) == (0, 0, 0)
    assert tuple(canvas.pixels[0, 2]) == (255, 255, 255)
    assert tuple(vyrn.canvas(2, 2, color="yellow").pixels[1, 1]) == tuple(rgb("yellow"))


def test_unknown_colour_is_an_error():
    canvas = Canvas(2, 2)
    with pytest.raises(Exception):
        canvas.dot(0, 0, "not-a-colour")
