#if (NT == PV)
#define PvNode 1
#define cutNode 0
#define name_NT(name) name##_PV
#else
#define PvNode 0
#define name_NT(name) name##_NonPV
#define beta (alpha+1)
#endif

#if PvNode
Value search_PV(Pos *pos, Stack *ss, Value alpha, Value beta, Depth depth)
#else
Value search_NonPV(Pos *pos, Stack *ss, Value alpha, Depth depth, int cutNode)
#endif
{
  int rootNode = PvNode && (ss-1)->ply == 0;

  assert(-VALUE_INFINITE <= alpha && alpha < beta && beta <= VALUE_INFINITE);
  assert(PvNode || (alpha == beta - 1));
  assert(DEPTH_ZERO < depth && depth < DEPTH_MAX);
  assert(!(PvNode && cutNode));

  Move pv[MAX_PLY+1], quietsSearched[64];
  TTEntry *tte;
  Key posKey;
  Move ttMove, move, excludedMove, bestMove;
  Depth extension, newDepth;
  Value bestValue, value, ttValue, eval, nullValue;
  int ttHit, inCheck, givesCheck, singularExtensionNode, improving;
  int captureOrPromotion, doFullDepthSearch, moveCountPruning;
  Piece moved_piece;
  int moveCount, quietCount;

  // Step 1. Initialize node
  inCheck = !!pos_checkers();
  moveCount = quietCount =  ss->moveCount = 0;
  bestValue = -VALUE_INFINITE;
  ss->ply = (ss-1)->ply + 1;

  // Check for the available remaining time
  if (load_rlx(pos->resetCalls)) {
    store_rlx(pos->resetCalls, 0);
    pos->callsCnt = 0;
  }
  if (++pos->callsCnt > 4096) {
    for (size_t idx = 0; idx < Threads.num_threads; idx++)
      store_rlx(Threads.pos[idx]->resetCalls, 1);

    check_time();
  }

  // Used to send selDepth info to GUI
  if (PvNode && pos->maxPly < ss->ply)
    pos->maxPly = ss->ply;

  if (!rootNode) {
    // Step 2. Check for aborted search and immediate draw
    if (load_rlx(Signals.stop) || is_draw(pos) || ss->ply >= MAX_PLY)
      return ss->ply >= MAX_PLY && !inCheck ? evaluate(pos)
                                            : DrawValue[pos_stm()];

    // Step 3. Mate distance pruning. Even if we mate at the next move our
    // score would be at best mate_in(ss->ply+1), but if alpha is already
    // bigger because a shorter mate was found upward in the tree then
    // there is no need to search because we will never beat the current
    // alpha. Same logic but with reversed signs applies also in the
    // opposite condition of being mated instead of giving mate. In this
    // case return a fail-high score.
#if PvNode
    alpha = max(mated_in(ss->ply), alpha);
    beta = min(mate_in(ss->ply+1), beta);
    if (alpha >= beta)
      return alpha;
#else
    if (alpha < mated_in(ss->ply))
      return mated_in(ss->ply);
    if (alpha >= mate_in(ss->ply+1))
      return alpha;
#endif
  }

  assert(0 <= ss->ply && ss->ply < MAX_PLY);

  ss->currentMove = (ss+1)->excludedMove = bestMove = 0;
  ss->counterMoves = NULL;
  (ss+1)->skipEarlyPruning = 0;
  (ss+2)->killers[0] = (ss+2)->killers[1] = 0;

  // Step 4. Transposition table lookup. We don't want the score of a
  // partial search to overwrite a previous full search TT value, so we
  // use a different position key in case of an excluded move.
  excludedMove = ss->excludedMove;
  posKey = pos_key() ^ (Key)excludedMove;
  tte = tt_probe(posKey, &ttHit);
  ttValue = ttHit ? value_from_tt(tte_value(tte), ss->ply) : VALUE_NONE;
  ttMove =  rootNode ? pos->rootMoves->move[pos->PVIdx].pv[0]
          : ttHit    ? tte_move(tte) : 0;

  // At non-PV nodes we check for an early TT cutoff.
  if (  !PvNode
      && ttHit
      && tte_depth(tte) >= depth
      && ttValue != VALUE_NONE // Possible in case of TT access race.
      && (ttValue >= beta ? (tte_bound(tte) & BOUND_LOWER)
                          : (tte_bound(tte) & BOUND_UPPER))) {
    ss->currentMove = ttMove; // Can be 0.

    // If ttMove is quiet, update killers, history, counter move on TT hit.
    if (ttValue >= beta && ttMove) {
      int d = depth / ONE_PLY;

      if (!is_capture_or_promotion(pos, ttMove)) {
        Value bonus = d * d + 2 * d - 2;
        update_stats(pos, ss, ttMove, NULL, 0, bonus);
      }

      // Extra penalty for a quiet TT move in previous ply when it gets refuted.
      if ((ss-1)->moveCount == 1 && !captured_piece()) {
        Value penalty = d * d + 4 * d + 1;
        Square prevSq = to_sq((ss-1)->currentMove);
        update_cm_stats(ss-1, piece_on(prevSq), prevSq, -penalty);
      }
    }
    return ttValue;
  }

  // Step 4a. Tablebase probe
  if (!rootNode && TB_Cardinality) {
    int piecesCnt = popcount(pieces());

    if (    piecesCnt <= TB_Cardinality
        && (piecesCnt <  TB_Cardinality || depth >= TB_ProbeDepth)
        &&  pos_rule50_count() == 0
        && !can_castle_cr(ANY_CASTLING)) {

      int found, v = TB_probe_wdl(pos, &found);

      if (found) {
        pos->tb_hits++;

        int drawScore = TB_UseRule50 ? 1 : 0;

        value =  v < -drawScore ? -VALUE_MATE + MAX_PLY + ss->ply
               : v >  drawScore ?  VALUE_MATE - MAX_PLY - ss->ply
                                :  VALUE_DRAW + 2 * v * drawScore;

        tte_save(tte, posKey, value_to_tt(value, ss->ply), BOUND_EXACT,
                 min(DEPTH_MAX - ONE_PLY, depth + 6 * ONE_PLY),
                 0, VALUE_NONE, tt_generation());

        return value;
      }
    }
  }

  // Step 5. Evaluate the position statically
  if (inCheck) {
    ss->staticEval = eval = VALUE_NONE;
    goto moves_loop;
  } else if (ttHit) {
    // Never assume anything on values stored in TT
    if ((ss->staticEval = eval = tte_eval(tte)) == VALUE_NONE)
      eval = ss->staticEval = evaluate(pos);

    // Can ttValue be used as a better position evaluation?
    if (ttValue != VALUE_NONE)
      if (tte_bound(tte) & (ttValue > eval ? BOUND_LOWER : BOUND_UPPER))
        eval = ttValue;
  } else {
    eval = ss->staticEval =
    (ss-1)->currentMove != MOVE_NULL ? evaluate(pos)
                                     : -(ss-1)->staticEval + 2 * Tempo;

    tte_save(tte, posKey, VALUE_NONE, BOUND_NONE, DEPTH_NONE, 0,
             ss->staticEval, tt_generation());
  }

  if (ss->skipEarlyPruning)
    goto moves_loop;

  // Step 6. Razoring (skipped when in check)
  if (   !PvNode
      &&  depth < 4 * ONE_PLY
      &&  !ttMove
      &&  eval + razor_margin[depth / ONE_PLY] <= alpha) {

    if (   depth <= ONE_PLY
        && eval + razor_margin[3 * ONE_PLY] <= alpha)
      return qsearch_NonPV_false(pos, ss, alpha, DEPTH_ZERO);

    Value ralpha = alpha - razor_margin[depth / ONE_PLY];
    Value v = qsearch_NonPV_false(pos, ss, ralpha, DEPTH_ZERO);
    if (v <= ralpha)
      return v;
  }

  // Step 7. Futility pruning: child node (skipped when in check)
  if (   !rootNode
      &&  depth < 7 * ONE_PLY
      &&  eval - futility_margin(depth) >= beta
      &&  eval < VALUE_KNOWN_WIN  // Do not return unproven wins
      &&  pos_non_pawn_material(pos_stm()))
    return eval - futility_margin(depth);

  // Step 8. Null move search with verification search (is omitted in PV nodes)
  if (   !PvNode
      &&  eval >= beta
      && (ss->staticEval >= beta - 35 * (depth / ONE_PLY - 6) || depth >= 13 * ONE_PLY)
      &&  pos_non_pawn_material(pos_stm())) {

    ss->currentMove = MOVE_NULL;
    ss->counterMoves = NULL;

    assert(eval - beta >= 0);

    // Null move dynamic reduction based on depth and value
    Depth R = ((823 + 67 * depth / ONE_PLY) / 256 + min((eval - beta) / PawnValueMg, 3)) * ONE_PLY;

    do_null_move(pos);
    ss->endMoves = (ss-1)->endMoves;
    (ss+1)->skipEarlyPruning = 1;
    nullValue = depth-R < ONE_PLY ? -qsearch_NonPV_false(pos, ss+1, -beta, DEPTH_ZERO)
                                  : - search_NonPV(pos, ss+1, -beta, depth-R, !cutNode);
    (ss+1)->skipEarlyPruning = 0;
    undo_null_move(pos);

    if (nullValue >= beta) {
      // Do not return unproven mate scores
      if (nullValue >= VALUE_MATE_IN_MAX_PLY)
         nullValue = beta;

      if (depth < 12 * ONE_PLY && abs(beta) < VALUE_KNOWN_WIN)
         return nullValue;

      // Do verification search at high depths
      ss->skipEarlyPruning = 1;
      Value v = depth-R < ONE_PLY ? qsearch_NonPV_false(pos, ss, beta-1, DEPTH_ZERO)
                                  :  search_NonPV(pos, ss, beta-1, depth-R, 0);
      ss->skipEarlyPruning = 0;

      if (v >= beta)
        return nullValue;
    }
  }

  // Step 9. ProbCut (skipped when in check)
  // If we have a good enough capture and a reduced search returns a value
  // much above beta, we can (almost) safely prune the previous move.
  if (   !PvNode
      &&  depth >= 5 * ONE_PLY
      &&  abs(beta) < VALUE_MATE_IN_MAX_PLY) {

    Value rbeta = min(beta + 200, VALUE_INFINITE);
    Depth rdepth = depth - 4 * ONE_PLY;

    assert(rdepth >= ONE_PLY);
    assert((ss-1)->currentMove != 0);
    assert((ss-1)->currentMove != 0);

    mp_init_pc(pos, ttMove, rbeta - ss->staticEval);

    while ((move = next_move(pos)))
      if (is_legal(pos, move)) {
        ss->currentMove = move;
        ss->counterMoves = &(*pos->counterMoveHistory)[moved_piece(move)][to_sq(move)];
        do_move(pos, move, gives_check(pos, ss, move));
        value = -search_NonPV(pos, ss+1, -rbeta, rdepth, !cutNode);
        undo_move(pos, move);
        if (value >= rbeta)
          return value;
      }
  }

  // Step 10. Internal iterative deepening (skipped when in check)
  if (    depth >= 6 * ONE_PLY
      && !ttMove
      && (PvNode || ss->staticEval + 256 >= beta))
  {
    Depth d = (3 * depth / (4 * ONE_PLY) - 2) * ONE_PLY;
    ss->skipEarlyPruning = 1;
#if PvNode
    search_PV(pos, ss, alpha, beta, d);
#else
    search_NonPV(pos, ss, alpha, d, cutNode);
#endif
    ss->skipEarlyPruning = 0;

    tte = tt_probe(posKey, &ttHit);
    ttMove = ttHit ? tte_move(tte) : 0;
  }

moves_loop: // When in check search starts from here.
  ;  // Avoid a compiler warning. A label must be followed by a statement.
  CounterMoveStats *cmh  = (ss-1)->counterMoves;
  CounterMoveStats *fmh  = (ss-2)->counterMoves;
  CounterMoveStats *fmh2 = (ss-4)->counterMoves;

  mp_init(pos, ttMove, depth);
  value = bestValue; // Workaround a bogus 'uninitialized' warning under gcc
  improving =   ss->staticEval >= (ss-2)->staticEval
          /* || ss->staticEval == VALUE_NONE Already implicit in the previous condition */
             ||(ss-2)->staticEval == VALUE_NONE;

  singularExtensionNode =   !rootNode
                         &&  depth >= 8 * ONE_PLY
                         &&  ttMove
                     /*  &&  ttValue != VALUE_NONE Already implicit in the next condition */
                         &&  abs(ttValue) < VALUE_KNOWN_WIN
                         && !excludedMove // Recursive singular search is not allowed
                         && (tte_bound(tte) & BOUND_LOWER)
                         &&  tte_depth(tte) >= depth - 3 * ONE_PLY;

  // Step 11. Loop through moves
  // Loop through all pseudo-legal moves until no moves remain or a beta cutoff occurs
  while ((move = next_move(pos))) {
    assert(move_is_ok(move));

    if (move == excludedMove)
      continue;

    // At root obey the "searchmoves" option and skip moves not listed
    // inRoot Move List. As a consequence any illegal move is also skipped.
    // In MultiPV mode we also skip PV moves which have been already
    // searched.
    if (rootNode) {
      size_t idx;
      for (idx = pos->PVIdx; idx < pos->rootMoves->size; idx++)
        if (pos->rootMoves->move[idx].pv[0] == move)
          break;
      if (idx == pos->rootMoves->size)
        continue;
    }

    ss->moveCount = ++moveCount;

    if (rootNode && pos->thread_idx == 0 && time_elapsed() > 3000) {
      char buf[16];
      IO_LOCK;
      printf("info depth %d currmove %s currmovenumber %d\n",
             depth / ONE_PLY,
             uci_move(buf, move, is_chess960()),
             moveCount + pos->PVIdx);
      fflush(stdout);
      IO_UNLOCK;
    }

    if (PvNode)
      (ss+1)->pv = NULL;

    extension = DEPTH_ZERO;
    captureOrPromotion = is_capture_or_promotion(pos, move);
    moved_piece = moved_piece(move);

    givesCheck = gives_check(pos, ss, move);

    moveCountPruning =   depth < 16 * ONE_PLY
                && moveCount >= FutilityMoveCounts[improving][depth / ONE_PLY];

    // Step 12. Extend checks
    if (    givesCheck
        && !moveCountPruning
        &&  see_test(pos, move, 0))
      extension = ONE_PLY;

    // Singular extension search. If all moves but one fail low on a search
    // of (alpha-s, beta-s), and just one fails high on (alpha, beta), then
    // that move is singular and should be extended. To verify this we do a
    // reduced search on all the other moves but the ttMove and if the
    // result is lower than ttValue minus a margin then we extend the ttMove.
    if (    singularExtensionNode
        &&  move == ttMove
        && !extension
        &&  is_legal(pos, move))
    {
      Value rBeta = ttValue - 2 * depth / ONE_PLY;
      Depth d = (depth / (2 * ONE_PLY)) * ONE_PLY;
      ss->excludedMove = move;
      ss->skipEarlyPruning = 1;
      Move cm = ss->countermove;
      value = search_NonPV(pos, ss, rBeta - 1, d, cutNode);
      ss->skipEarlyPruning = 0;
      ss->excludedMove = 0;

      if (value < rBeta)
        extension = ONE_PLY;

      // The call to search_NonPV with the same value of ss messed up our
      // move picker data. So we fix it.
      mp_init(pos, ttMove, depth);
      ss->stage++;
      ss->countermove = cm; // pedantic
    }

    // Update the current move (this must be done after singular extension search)
    newDepth = depth - ONE_PLY + extension;

    // Step 13. Pruning at shallow depth
    if (   !rootNode
        && !inCheck
        &&  bestValue > VALUE_MATED_IN_MAX_PLY)
    {
      if (   !captureOrPromotion
          && !givesCheck
          && !advanced_pawn_push(pos, move))
      {
        // Move count based pruning
        if (moveCountPruning)
          continue;

        // Reduced depth of the next LMR search
        int lmrDepth = max(newDepth - reduction(improving, depth, moveCount, NT), DEPTH_ZERO) / ONE_PLY;

        // Countermoves based pruning
        if (   lmrDepth < 3
            && (!cmh  || (*cmh )[moved_piece][to_sq(move)] < 0)
            && (!fmh  || (*fmh )[moved_piece][to_sq(move)] < 0)
            && (!fmh2 || (*fmh2)[moved_piece][to_sq(move)] < 0 || (cmh && fmh)))
          continue;

        // Futility pruning: parent node
        if (   lmrDepth < 7
            && ss->staticEval + 256 + 200 * lmrDepth <= alpha)
          continue;

        // Prune moves with negative SEE at low depths and below a decreasing
        // threshold at higher depths.
        if (   lmrDepth < 8
            && !see_test(pos, move, -35 * lmrDepth * lmrDepth))
          continue;
      }
      else if (   depth < 7 * ONE_PLY
               && !see_test(pos, move, -35 * depth / ONE_PLY * depth / ONE_PLY))
        continue;
    }

    // Speculative prefetch as early as possible
    prefetch(tt_first_entry(key_after(pos, move)));

    // Check for legality just before making the move
    if (!rootNode && !is_legal(pos, move)) {
      ss->moveCount = --moveCount;
      continue;
    }

    ss->currentMove = move;
    ss->counterMoves = &(*pos->counterMoveHistory)[moved_piece][to_sq(move)];

    // Step 14. Make the move
    do_move(pos, move, givesCheck);

    // Step 15. Reduced depth search (LMR). If the move fails high it will be
    // re-searched at full depth.
    if (    depth >= 3 * ONE_PLY
        &&  moveCount > 1
        && (!captureOrPromotion || moveCountPruning))
    {
      Depth r = reduction(improving, depth, moveCount, NT);
      if (captureOrPromotion)
        r -= r ? ONE_PLY : DEPTH_ZERO;
      else {
        // Increase reduction for cut nodes
        if (cutNode)
          r += 2 * ONE_PLY;

        // Decrease reduction for moves that escape a capture. Filter out
        // castling moves, because they are coded as "king captures rook" and
        // hence break make_move(). Also use see() instead of see_sign(),
        // because the destination square is empty.
        else if (   type_of_m(move) == NORMAL
                 && type_of_p(piece_on(to_sq(move))) != PAWN
                 && !see_test(pos, make_move(to_sq(move), from_sq(move)), 0))
          r -= 2 * ONE_PLY;

        // Decrease/increase reduction for moves with a good/bad history
        Value val =  (*pos->history)[moved_piece][to_sq(move)]
                   + (cmh  ? (*cmh )[moved_piece][to_sq(move)] : 0)
                   + (fmh  ? (*fmh )[moved_piece][to_sq(move)] : 0)
                   + (fmh2 ? (*fmh2)[moved_piece][to_sq(move)] : 0)
                   + ft_get(*pos->fromTo, pos_stm() ^ 1, move);
        int rHist = (val - 8000) / 20000;
        r = max(DEPTH_ZERO, (r / ONE_PLY - rHist) * ONE_PLY);
      }

      Depth d = max(newDepth - r, ONE_PLY);

      value = -search_NonPV(pos, ss+1, -(alpha+1), d, 1);

      doFullDepthSearch = (value > alpha && d != newDepth);

    } else
      doFullDepthSearch = !PvNode || moveCount > 1;

    // Step 16. Full depth search when LMR is skipped or fails high
    if (doFullDepthSearch)
        value = newDepth <   ONE_PLY ?
                          givesCheck ? -qsearch_NonPV_true(pos, ss+1, -(alpha+1), DEPTH_ZERO)
                                     : -qsearch_NonPV_false(pos, ss+1, -(alpha+1), DEPTH_ZERO)
                                     : - search_NonPV(pos, ss+1, -(alpha+1), newDepth, !cutNode);

    // For PV nodes only, do a full PV search on the first move or after a fail
    // high (in the latter case search only if value < beta), otherwise let the
    // parent node fail low with value <= alpha and try another move.
    if (PvNode && (moveCount == 1 || (value > alpha && (rootNode || value < beta)))) {
      (ss+1)->pv = pv;
      (ss+1)->pv[0] = 0;

      value = newDepth <   ONE_PLY ?
                        givesCheck ? -qsearch_PV_true(pos, ss+1, -beta, -alpha, DEPTH_ZERO)
                                   : -qsearch_PV_false(pos, ss+1, -beta, -alpha, DEPTH_ZERO)
                                   : - search_PV(pos, ss+1, -beta, -alpha, newDepth);
    }

    // Step 17. Undo move
    undo_move(pos, move);

    assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

    // Step 18. Check for a new best move
    // Finished searching the move. If a stop occurred, the return value of
    // the search cannot be trusted, and we return immediately without
    // updating best move, PV and TT.
    if (load_rlx(Signals.stop))
      return 0;

    if (rootNode) {
      RootMove *rm = NULL;
      for (size_t idx = 0; idx < pos->rootMoves->size; idx++)
        if (pos->rootMoves->move[idx].pv[0] == move) {
          rm = &pos->rootMoves->move[idx];
          break;
        }

      // PV move or new best move ?
      if (moveCount == 1 || value > alpha) {
        rm->score = value;
        rm->pv_size = 1;

        assert((ss+1)->pv);

        for (Move *m = (ss+1)->pv; *m; ++m)
          rm->pv[rm->pv_size++] = *m;

        // We record how often the best move has been changed in each
        // iteration. This information is used for time management: When
        // the best move changes frequently, we allocate some more time.
        if (moveCount > 1 && pos->thread_idx == 0)
          mainThread.bestMoveChanges++;
      } else
        // All other moves but the PV are set to the lowest value: this is
        // not a problem when sorting because the sort is stable and the
        // move position in the list is preserved - just the PV is pushed up.
        rm->score = -VALUE_INFINITE;
    }

    if (value > bestValue) {
      bestValue = value;

      if (value > alpha) {
        // If there is an easy move for this position, clear it if unstable
        if (    PvNode
            &&  pos->thread_idx == 0
            &&  easy_move_get(pos_key())
            && (move != easy_move_get(pos_key()) || moveCount > 1))
          easy_move_clear();

        bestMove = move;

        if (PvNode && !rootNode) // Update pv even in fail-high case
          update_pv(ss->pv, move, (ss+1)->pv);

        if (PvNode && value < beta) // Update alpha! Always alpha < beta
          alpha = value;
        else {
          assert(value >= beta); // Fail high
          break;
        }
      }
    }

    if (!captureOrPromotion && move != bestMove && quietCount < 64)
      quietsSearched[quietCount++] = move;
  }

  // The following condition would detect a stop only after move loop has
  // been completed. But in this case bestValue is valid because we have
  // fully searched our subtree, and we can anyhow save the result in TT.
  /*
  if (Signals.stop)
    return VALUE_DRAW;
  */

  // Step 20. Check for mate and stalemate
  // All legal moves have been searched and if there are no legal moves,
  // it must be a mate or a stalemate. If we are in a singular extension
  // search then return a fail low score.
  if (!moveCount)
    bestValue = excludedMove ? alpha
               :     inCheck ? mated_in(ss->ply) : DrawValue[pos_stm()];
  else if (bestMove) {
    int d = depth / ONE_PLY;

    // Quiet best move: update killers, history and countermoves.
    if (!is_capture_or_promotion(pos, bestMove)) {
      Value bonus = d * d + 2 * d - 2;
      update_stats(pos, ss, bestMove, quietsSearched, quietCount, bonus);
    }

    // Extra penalty for a quiet TT move in previous ply when it gets refuted.
    if ((ss-1)->moveCount == 1 && !captured_piece()) {
      Value penalty = d * d + 4 * d + 1;
      Square prevSq = to_sq((ss-1)->currentMove);
      update_cm_stats(ss-1, piece_on(prevSq), prevSq, -penalty);
    }
  }
  // Bonus for prior countermove that caused the fail low.
  else if (    depth >= 3 * ONE_PLY
           && !captured_piece()
           && move_is_ok((ss-1)->currentMove))
  {
    int d = depth / ONE_PLY;
    Value bonus = d * d + 2 * d - 2;
    Square prevSq = to_sq((ss-1)->currentMove);
    update_cm_stats(ss-1, piece_on(prevSq), prevSq, bonus);
  }

  tte_save(tte, posKey, value_to_tt(bestValue, ss->ply),
           bestValue >= beta ? BOUND_LOWER :
           PvNode && bestMove ? BOUND_EXACT : BOUND_UPPER,
           depth, bestMove, ss->staticEval, tt_generation());

  assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);

  return bestValue;
}

#undef PvNode
#undef name_NT
#ifdef cutNode
#undef cutNode
#endif
#ifdef beta
#undef beta
#endif

