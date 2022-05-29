.global cas
cas:
    lr.w t0, (a0) # Load original value.
    bne t0, a1, fail # Doesn’t match, so fail.
    sc.w a0, a2, (a0) # Try to update.
    jr ra # Return.
fail:
    li a0, 1 # Set return to failure.
    jr ra # Return.