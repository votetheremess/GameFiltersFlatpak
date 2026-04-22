data_dir = get_option('datadir')
vulkan_layer_dir = join_paths(data_dir, 'vulkan', 'implicit_layer.d')

configure_file(
    input: 'lumen.json.in',
    output: 'lumen.json',
    copy: true,
    install: true,
    install_dir: vulkan_layer_dir,
)

install_data(
    'lumen.conf',
    install_dir: join_paths(data_dir, 'lumen'),
)
