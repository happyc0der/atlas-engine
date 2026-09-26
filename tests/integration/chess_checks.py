#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end checks that run the chess application (ADR-0018).

The rules are tested in the library; what only the whole program can show is that a game
played through the application's own command line reaches the result the rules say, that a
move the rules decline stops a scripted game with an error naming it, and that the program
starts and stops cleanly with a window and without one.

The games are real: Scholar's mate, Loyd's ten-move stalemate, and a repetition by knights.
Their final positions are facts about chess, so a regression anywhere between the command line
and the board shows up as a wrong FEN rather than as a wrong number nobody can check by hand.

Run one with:
    python3 tests/integration/chess_checks.py --binary <path> --case <name>
"""

from __future__ import annotations

import argparse
import re
import sys

from harness import CheckFailed, Session, expect_contains, expect_exit, output_of, run


def game(binary: str, moves: str, *extra: str):
    return run(binary, ["--headless", "--moves", moves, *extra])


def result_line(text: str) -> str:
    match = re.search(r"^result: (.+)$", text, re.MULTILINE)
    if not match:
        raise CheckFailed(f"no result line\n--- output ---\n{text}")
    return match.group(1)


def position_line(text: str) -> str:
    match = re.search(r"^position: (.+)$", text, re.MULTILINE)
    if not match:
        raise CheckFailed(f"no position line\n--- output ---\n{text}")
    return match.group(1)


def check_version(binary: str) -> None:
    result = run(binary, ["--version"])
    expect_exit(result, 0, "--version")
    expect_contains(output_of(result), "Atlas", "the build identity")


def check_help(binary: str) -> None:
    result = run(binary, ["--help"])
    expect_exit(result, 0, "--help")
    expect_contains(output_of(result), "--moves", "the usage block")


def check_scholars_mate(binary: str) -> None:
    result = game(binary, "e2e4,e7e5,f1c4,b8c6,d1h5,g8f6,h5f7")
    expect_exit(result, 0, "Scholar's mate")
    text = output_of(result)
    if result_line(text) != "white wins by checkmate":
        raise CheckFailed(f"expected a white win, got {result_line(text)!r}")
    if position_line(text) != "r1bqkb1r/pppp1Qpp/2n2n2/4p3/2B1P3/8/PPPP1PPP/RNB1K1NR b KQkq - 0 4":
        raise CheckFailed(f"the final position is wrong: {position_line(text)}")
    expect_contains(text, "plies=7", "every move was applied")


def check_loyds_stalemate(binary: str) -> None:
    # Sam Loyd's ten-move stalemate: black has pieces left and no legal move, and is not in
    # check. The longest scripted game here, and the one that exercises the most captures.
    moves = ("e2e3,a7a5,d1h5,a8a6,h5a5,h7h5,h2h4,a6h6,a5c7,f7f6,c7d7,e8f7,d7b7,d8d3,"
             "b7b8,d3h7,b8c8,f7g6,c8e6")
    result = game(binary, moves)
    expect_exit(result, 0, "Loyd's stalemate")
    text = output_of(result)
    if result_line(text) != "draw by stalemate":
        raise CheckFailed(f"expected stalemate, got {result_line(text)!r}")
    expect_contains(text, "plies=19", "every move was applied")


def check_threefold_repetition(binary: str) -> None:
    # Knights out and back twice: the starting position occurs a third time on the eighth ply.
    result = game(binary, "g1f3,g8f6,f3g1,f6g8,g1f3,g8f6,f3g1,f6g8")
    expect_exit(result, 0, "a repetition")
    if result_line(output_of(result)) != "draw by threefold repetition":
        raise CheckFailed(f"expected a repetition draw, got {result_line(output_of(result))!r}")


def check_declined_move_stops_the_script(binary: str) -> None:
    # A pawn cannot move forward into a pawn. The rules decline it inside the tick; a scripted
    # game treats that as an error rather than as a move that silently did nothing.
    result = game(binary, "e2e4,e7e5,e4e5")
    expect_exit(result, 1, "a declined move")
    text = output_of(result)
    expect_contains(text, "move 3 (e4e5) was declined", "the error names the move")
    expect_contains(text, "plies=2", "the two legal moves were applied first")


def check_move_after_the_end_is_declined(binary: str) -> None:
    # After mate nothing moves, whoever's turn the board says it is.
    result = game(binary, "f2f3,e7e5,g2g4,d8h4,a2a3")
    expect_exit(result, 1, "a move after checkmate")
    expect_contains(output_of(result), "black wins by checkmate", "the game had ended")
    expect_contains(output_of(result), "(a2a3) was declined", "the move after it was declined")


def check_fen_is_validated(binary: str) -> None:
    kingless = run(binary, ["--headless", "--fen", "8/8/8/8/8/8/8/8 w - - 0 1"])
    expect_exit(kingless, 1, "a position with no kings")
    expect_contains(output_of(kingless), "kings", "the refusal says what is wrong")

    malformed = run(binary, ["--headless", "--fen", "not a position"])
    expect_exit(malformed, 1, "a malformed FEN")

    # And a legal one is played from: mate in one from a set-up position.
    mate_in_one = run(binary, ["--headless", "--fen", "6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1",
                               "--moves", "a1a8"])
    expect_exit(mate_in_one, 0, "mate in one from a FEN")
    if result_line(output_of(mate_in_one)) != "white wins by checkmate":
        raise CheckFailed("a back-rank mate from a FEN was not recognised")


def check_same_game_same_hash(binary: str) -> None:
    moves = "d2d4,d7d5,c2c4,e7e6,b1c3,g8f6"
    first = game(binary, moves)
    second = game(binary, moves)
    expect_exit(first, 0, "first run")
    expect_exit(second, 0, "second run")
    hashes = [re.search(r"state hash: (0x[0-9a-f]+)", output_of(r)) for r in (first, second)]
    if not all(hashes) or hashes[0].group(1) != hashes[1].group(1):
        raise CheckFailed("the same game hashed differently on two runs")


def check_text_check(binary: str) -> None:
    # Every chess key resolves against the two tables actually loaded: the engine's, and the
    # application's own beside it (ADR-0018). A game adding its strings must not have to edit the
    # engine's table, and this is the check that it did not.
    result = run(binary, ["--text-check"])
    expect_exit(result, 0, "--text-check")
    expect_contains(output_of(result), "all resolved in 'en'", "every chess key resolved")


def state_hash(text: str) -> str:
    match = re.search(r"^state hash: (0x[0-9a-f]+)$", text, re.MULTILINE)
    if not match:
        raise CheckFailed(f"no state hash line\n--- output ---\n{text}")
    return match.group(1)


def check_socket_game_to_checkmate(binary: str) -> None:
    """Two processes play Scholar's mate over a socket, and both finish having agreed."""
    # Each side plays its own colour and its own moves, and neither knows the other's list: a
    # move reaches the far side only as a command in a turn. The game ends where the rules say,
    # and both sides finish the session there and exit zero (ADR-0020) — the thing M21 existed
    # to make possible and this case exists to prove.
    with Session(binary, []) as session:
        session.start(listener_extra=["--moves", "e2e4,f1c4,d1h5,h5f7"],
                      connector_extra=["--moves", "e7e5,b8c6,g8f6"])
        white_code, black_code = session.wait()
    white = session.listener_text()
    black = session.connector_text()
    if white_code != 0 or black_code != 0:
        raise CheckFailed(f"a side did not exit zero (white {white_code}, black {black_code})\n"
                          f"white:\n{white}\nblack:\n{black}")
    for name, text in (("white", white), ("black", black)):
        if result_line(text) != "white wins by checkmate":
            raise CheckFailed(f"{name} saw {result_line(text)!r}\n{text}")
        if position_line(text) != "r1bqkb1r/pppp1Qpp/2n2n2/4p3/2B1P3/8/PPPP1PPP/RNB1K1NR b KQkq - 0 4":
            raise CheckFailed(f"{name} ended in the wrong position: {position_line(text)}")
        expect_contains(text, "session: finished at tick", f"{name} finished the session")
        expect_contains(text, "plies=7", f"{name} saw all seven plies")
    if state_hash(white) != state_hash(black):
        raise CheckFailed(f"the two sides hashed differently: {state_hash(white)} and "
                          f"{state_hash(black)}")


def check_socket_partner_leaving_ends_the_game(binary: str) -> None:
    """A side that stops before the game is over ends it for the other, who says so."""
    # White has one move to give and then runs out on its next turn, which is an error on its
    # side; black must then hear that its partner left rather than wait for a move that will
    # never come, and must not exit zero — a game that was abandoned did not succeed.
    with Session(binary, []) as session:
        session.start(listener_extra=["--moves", "e2e4"],
                      connector_extra=["--moves", "e7e5,b8c6,g8f6"])
        white_code, black_code = session.wait()
    white = session.listener_text()
    black = session.connector_text()
    if white_code == 0 or black_code == 0:
        raise CheckFailed(f"a side exited zero from an abandoned game "
                          f"(white {white_code}, black {black_code})\n"
                          f"white:\n{white}\nblack:\n{black}")
    expect_contains(white, "--moves ran out", "white says why it stopped")
    if "left" not in black and "disconnected" not in black and "heard from" not in black:
        raise CheckFailed(f"black did not say its partner had gone\n{black}")


def check_socket_sides_must_start_alike(binary: str) -> None:
    """Two sides set up from different positions are refused at the handshake."""
    with Session(binary, []) as session:
        session.start(listener_extra=["--fen", "6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1"],
                      connector_extra=[])
        white_code, black_code = session.wait()
    if white_code == 0 or black_code == 0:
        raise CheckFailed("two sides starting from different positions were allowed to play\n"
                          f"white:\n{session.listener_text()}\nblack:\n{session.connector_text()}")


def check_window_under_dummy_driver(binary: str) -> None:
    # A window, no device: the event loop runs and the program shuts down in order.
    result = run(binary, ["--video-driver", "dummy", "--no-render", "--frames", "10",
                          "--moves", "e2e4"])
    expect_exit(result, 0, "a window under the dummy driver")
    text = output_of(result)
    expect_contains(text, "plies=1", "the scripted move was applied before the window opened")
    expect_contains(text, "shutdown", "the program shut down in order")


def opponent(binary: str, *extra: str):
    return run(binary, ["--headless", "--mod", "chess_opponent.wasm", *extra])


def check_mod_plays_itself_to_the_end(binary: str) -> None:
    """The opponent against itself (ADR-0023): a legal game to a result, the same one twice."""
    first = opponent(binary, "--mod-plays", "both")
    expect_exit(first, 0, "the mod playing both sides")
    text = output_of(first)
    # A result the rules gave, not the bound: the game ended by mate, stalemate, the fifty-move
    # rule, repetition or material, and every move the mod submitted was applied.
    if result_line(text) == "ongoing":
        raise CheckFailed(f"the game did not end\n{text}")
    expect_contains(text, "plays both sides", "the mod played both sides")
    expect_contains(text, "still running", "the mod was never disabled")
    if "declined" in text:
        raise CheckFailed(f"a move the mod submitted was declined\n{text}")
    submitted = re.search(r"(\d+) move\(s\) submitted", text)
    plies = re.search(r"^chess: plies=(\d+)", text, re.MULTILINE)
    if not submitted or not plies or submitted.group(1) != plies.group(1):
        raise CheckFailed(f"every ply should be a move the mod submitted\n{text}")

    # Counted in moves, not time: nothing about the machine or the moment can change the game.
    second = opponent(binary, "--mod-plays", "both")
    expect_exit(second, 0, "the same game again")
    if state_hash(output_of(second)) != state_hash(text):
        raise CheckFailed("two runs of the mod against itself played different games")


def check_mod_answers_a_person(binary: str) -> None:
    """A person's scripted moves as white; the mod answers each, and takes a queen it is given."""
    # Scholar's mate, attempted against a defender that sees two plies. After Qxf7+ the queen is
    # defended by nothing, so the king takes it.
    result = opponent(binary, "--moves", "e2e4,d1h5,f1c4,h5f7")
    expect_exit(result, 0, "a person against the mod")
    text = output_of(result)
    expect_contains(text, "chess: plies=8", "every scripted move was answered")
    expect_contains(text, "ply 8: e8f7 (the mod)", "the mod took the queen")
    expect_contains(text, "plays black", "the mod played black by default")
    if text.count("(the mod)") != 4:
        raise CheckFailed(f"expected four moves by the mod\n{text}")


def check_mod_options_are_checked(binary: str) -> None:
    for args, needle in (
        (["--headless", "--mod-plays", "white"], "--mod-plays needs --mod"),
        (["--headless", "--mod", "chess_opponent.wasm", "--mod-plays", "red"], "white, black or both"),
        (["--headless", "--mod", "chess_opponent.wasm", "--listen"], "local game"),
        (["--headless", "--mod", "chess_opponent.wasm", "--mod-plays", "both", "--moves", "e2e4"],
         "nobody to play them"),
        (["--headless", "--mod", "no_such_mod.wasm"], "mod"),
        (["--headless", "--mod", "../../etc/passwd"], "mod"),
    ):
        result = run(binary, args)
        expect_exit(result, 1, " ".join(args))
        expect_contains(output_of(result), needle, " ".join(args))


def check_mod_plays_in_a_window(binary: str) -> None:
    # A window, no device, a tick a frame: the person's move goes in, and the mod answers within
    # the frames the run has.
    result = run(binary, ["--video-driver", "dummy", "--no-render", "--frames", "120",
                          "--mod", "chess_opponent.wasm", "--moves", "e2e4"])
    expect_exit(result, 0, "the mod in a window under the dummy driver")
    text = output_of(result)
    expect_contains(text, "ply 2:", "the mod answered in the window")
    expect_contains(text, "(the mod)", "the answer was the mod's")
    expect_contains(text, "shutdown", "the program shut down in order")


CASES = {
    "version": check_version,
    "help": check_help,
    "scholars_mate": check_scholars_mate,
    "loyds_stalemate": check_loyds_stalemate,
    "threefold_repetition": check_threefold_repetition,
    "declined_move_stops_the_script": check_declined_move_stops_the_script,
    "move_after_the_end_is_declined": check_move_after_the_end_is_declined,
    "fen_is_validated": check_fen_is_validated,
    "same_game_same_hash": check_same_game_same_hash,
    "text_check": check_text_check,
    "socket_game_to_checkmate": check_socket_game_to_checkmate,
    "socket_partner_leaving_ends_the_game": check_socket_partner_leaving_ends_the_game,
    "socket_sides_must_start_alike": check_socket_sides_must_start_alike,
    "window_under_dummy_driver": check_window_under_dummy_driver,
    "mod_plays_itself_to_the_end": check_mod_plays_itself_to_the_end,
    "mod_answers_a_person": check_mod_answers_a_person,
    "mod_options_are_checked": check_mod_options_are_checked,
    "mod_plays_in_a_window": check_mod_plays_in_a_window,
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", required=True, help="path to atlas_chess")
    parser.add_argument("--case", required=True, choices=sorted(CASES), help="which check")
    args = parser.parse_args()
    try:
        CASES[args.case](args.binary)
    except CheckFailed as failed:
        print(f"FAILED {args.case}: {failed}", file=sys.stderr)
        return 1
    print(f"ok {args.case}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
