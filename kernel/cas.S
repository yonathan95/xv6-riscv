.global cas
cas:
    lr.w t0, (a0)
    bne t0, a1, fail
    sc.w a0, a2, (a0)
    jr ra
fail:
    li a0, 1
    jr ra