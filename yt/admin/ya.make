RECURSE(
    dashboard_generator
    dashboards
    timbertruck
)

IF (NOT OPENSOURCE)
    RECURSE(
        abc_integration_client
        acl_dumper
        cms
        common
        core
        core3
        devil-hulk
        drive_monitor
        gh_ci_vm_image_builder
        hulk
        hwinfo
        infra_cli
        infra_noc
        libs
        luigi
        scheduler_codicils
        sevenpct
        shiva2
        snapshot_processing
        stateless-service-controller
        trash_recovery
        yt-tag-table
        yt_logs
        ytcfgen
        ytdyncfgen
    )
ENDIF()
