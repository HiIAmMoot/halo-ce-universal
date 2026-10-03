def pytest_configure(config):
    config.addinivalue_line("markers", "slow: takes about half a minute; kept in the full and the mutation runs")
