#!/usr/bin/env python3

PROFILES = {
    "z7020_small": {
        "HLS_PROFILE_MACRO": "FPGA_CPIM_HLS_PROFILE_Z7020_SMALL",
        "MAX_VARS": 128,
        "MAX_CONSTRAINTS": 512,
        "MAX_DOMAIN": 32,
        "MAX_WORDS": 1,
        "MAX_WORLDS": 1,
        "REVISION_TILES": 1,
        "OWNER_TILES": 1,
        "QUEUE_DEPTH": 512,
    },
    "z7020_probe2": {
        "HLS_PROFILE_MACRO": "FPGA_CPIM_HLS_PROFILE_Z7020_PROBE2",
        "MAX_VARS": 128,
        "MAX_CONSTRAINTS": 256,
        "MAX_DOMAIN": 32,
        "MAX_WORDS": 1,
        "MAX_WORLDS": 2,
        "REVISION_TILES": 1,
        "OWNER_TILES": 1,
        "QUEUE_DEPTH": 512,
    },
}


def fits_profile(profile, vars_count, constraints, max_domain):
    return (
        vars_count <= profile["MAX_VARS"]
        and constraints <= profile["MAX_CONSTRAINTS"]
        and max_domain <= profile["MAX_DOMAIN"]
    )


def profile_fit_map(vars_count, constraints, max_domain):
    return {
        name: fits_profile(profile, vars_count, constraints, max_domain)
        for name, profile in PROFILES.items()
    }
