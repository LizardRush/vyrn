# inputs.py: what the AI sees. The fake runner game sends these numbers over Vyrn Link.
import vyrn

def see():
    game = vyrn.game                           # the newest state from the game ({} until it connects)
    ahead = list(game.get("ahead", []))[:6]    # the next spikes: distance, height, kind for each
    ahead += [0] * (6 - len(ahead))            # zeros when there are fewer
    return {
        "height": game.get("y", 0) / 100,      # how high the runner is
        "speed": game.get("speed_y", 0) / 10,  # going up (+) or falling (-)
        "on ground": game.get("on_ground", 0), # 1 on the ground, 0 in the air
        "next spike": ahead[0] / 100,          # how far in front of the runner
        "spike after": ahead[3] / 100,         # and the one after it
    }
