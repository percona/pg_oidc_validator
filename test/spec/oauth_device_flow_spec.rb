require_relative 'spec_helper'
require 'timeout'

RSpec.describe 'OAuth Device Flow Authentication' do
  include TestHelpers

  before(:all) do
    puts "\n=== Setting up test environment ==="
    setup_postgres
    setup_keycloak
  end

  after(:all) do
    puts "\n=== Cleaning up test environment ==="
    cleanup_postgres
    cleanup_keycloak
  end

  it 'has Keycloak running and accessible' do
    stdout, = run_command("curl -k -s #{TestHelpers::KEYCLOAK_URL}/.well-known/openid-configuration")
    expect(stdout).to include('issuer')
    expect(stdout).to include('jwks_uri')
  end

  it 'successfully authenticates via OAuth device flow' do
    connection_string = [
      "host=127.0.0.1",
      "port=#{TestHelpers::PGPORT}",
      "dbname=postgres",
      "user=testuser",
      "oauth_issuer=#{TestHelpers::KEYCLOAK_URL}",
      "oauth_client_id=pgtest"
    ].join(' ')

    result = execute_psql_with_oauth(connection_string, 'SELECT current_user;')

    puts "\n=== psql output ===\n#{result[:output]}\n==================\n"

    expect(result[:exit_status]).to eq(0), "psql should exit with status 0, got #{result[:exit_status]}"

    errors = verify_oauth_query_output(result[:output])
    expect(errors).to be_empty, "Output verification failed:\n#{errors.join("\n")}"
  end
end
