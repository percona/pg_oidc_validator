require 'open3'
require 'fileutils'
require 'tmpdir'

RSpec.configure do |config|
  config.formatter = :documentation
  config.color = true
  config.fail_fast = false
end

module TestHelpers
  SCRIPT_DIR = File.expand_path('..', __dir__)
  PROJECT_ROOT = File.expand_path('../..', __dir__)
  PGDATA = File.join(SCRIPT_DIR, 'pgdata')
  PGPORT = 5432
  KEYCLOAK_PORT = 8443
  KEYCLOAK_URL = "https://localhost:#{KEYCLOAK_PORT}/realms/pgrealm"
  KEYCLOAK_USER = "testuser"
  KEYCLOAK_PASSWORD = "asdfasdf"

  def pgbin
    @pgbin ||= ENV['PGBIN'] ? "#{ENV['PGBIN']}/" : ""
  end

  def run_command(cmd, allow_failure: false)
    stdout, stderr, status = Open3.capture3(cmd)
    raise "Command failed: #{cmd}\n#{stderr}" unless allow_failure || status.success?
    [stdout, stderr, status]
  end

  def wait_for(timeout: 30, message: "Condition")
    timeout.times do
      return if yield
      sleep 1
    end
    raise "Timeout waiting for: #{message}"
  end

  def setup_postgres
    FileUtils.rm_rf(PGDATA)
    run_command("#{pgbin}initdb -D #{PGDATA}")

    File.open("#{PGDATA}/postgresql.conf", 'a') do |f|
      f.write(File.read(File.join(SCRIPT_DIR, 'postgresql.conf')))
    end

    run_command("#{pgbin}pg_ctl -D #{PGDATA} -l #{File.join(SCRIPT_DIR, 'postgres.log')} start")

    run_command("#{pgbin}createdb -p #{PGPORT} -h 127.0.0.1 postgres", allow_failure: true)
    run_command("#{pgbin}psql -p #{PGPORT} -h 127.0.0.1 -d postgres -c 'CREATE ROLE testuser LOGIN SUPERUSER;'", allow_failure: true)

    FileUtils.cp(File.join(SCRIPT_DIR, 'pg_hba.conf'), "#{PGDATA}/pg_hba.conf")
    FileUtils.cp(File.join(SCRIPT_DIR, 'pg_ident.conf'), "#{PGDATA}/pg_ident.conf")
    run_command("#{pgbin}pg_ctl -D #{PGDATA} reload")
    sleep 1
  end

  def cleanup_postgres
    run_command("#{pgbin}pg_ctl -D #{PGDATA} stop -m fast", allow_failure: true) if File.directory?(PGDATA)
    FileUtils.rm_rf(PGDATA)
  end

  def container_runtime
    @container_runtime ||= begin
      # Check if podman is available, fallback to docker
      if system("which podman > /dev/null 2>&1")
        "podman"
      elsif system("which docker > /dev/null 2>&1")
        "docker"
      else
        raise "Neither podman nor docker is available"
      end
    end
  end

  def setup_keycloak
    run_command("#{container_runtime} ps -q --filter 'ancestor=quay.io/keycloak/keycloak:latest' | xargs -r #{container_runtime} stop", allow_failure: true)
    sleep 2

    cmd = [
      "#{container_runtime} run -d --rm",
      "-p #{KEYCLOAK_PORT}:8443",
      "-e KC_BOOTSTRAP_ADMIN_USERNAME=admin",
      "-e KC_BOOTSTRAP_ADMIN_PASSWORD=admin",
      "-e KC_HTTPS_CERTIFICATE_FILE=/keys/crt.pem",
      "-e KC_HTTPS_CERTIFICATE_KEY_FILE=/keys/key.pem",
      "-v #{File.join(SCRIPT_DIR, 'import')}:/opt/keycloak/data/import",
      "-v #{File.join(SCRIPT_DIR, 'keys')}:/keys/",
      "quay.io/keycloak/keycloak:latest",
      "start-dev --import-realm"
    ].join(' ')

    run_command(cmd)

    wait_for(timeout: 60, message: "Keycloak to start") do
      run_command("curl -k -s #{KEYCLOAK_URL}/.well-known/openid-configuration", allow_failure: true)[2].success?
    end
  end

  def cleanup_keycloak
    run_command("#{container_runtime} ps -q --filter 'ancestor=quay.io/keycloak/keycloak:latest' | xargs -r #{container_runtime} stop", allow_failure: true)
  end

  def extract_device_code(output)
    output.lines.each do |line|
      if line =~ /Visit (https[^ ]*) and enter the code: ([A-Z0-9-]+)/
        return { url: $1, code: $2 }
      end
    end
    nil
  end

  def complete_device_flow(device_url, device_code)
    cmd = [
      "ruby #{File.join(SCRIPT_DIR, 'keycloak_device_flow.rb')}",
      "--user #{KEYCLOAK_USER}",
      "--password #{KEYCLOAK_PASSWORD}",
      "--code #{device_code}",
      "--url #{device_url}",
      "--headless"
    ].join(' ')

    stdout, stderr, status = run_command(cmd)

    output_file = File.join(SCRIPT_DIR, 'selenium_output.log')
    File.write(output_file, "STDOUT:\n#{stdout}\n\nSTDERR:\n#{stderr}\n")

    [stdout, stderr, status]
  end

  def execute_psql_with_oauth(connection_string, query, timeout: 30)
    cmd = "#{pgbin}psql \"#{connection_string}\" -c '#{query}'"

    stdin, stdout_stderr, wait_thread = Open3.popen2e(cmd, chdir: PROJECT_ROOT)
    stdin.close

    output_buffer = ""
    device_info = nil

    reader_thread = Thread.new do
      stdout_stderr.each_line do |line|
        output_buffer << line
      end
    end

    begin
      Timeout.timeout(10) do
        loop do
          device_info = extract_device_code(output_buffer)
          break if device_info
          sleep 0.5
        end
      end
    rescue Timeout::Error
      wait_thread.kill
      reader_thread.kill
      raise "Device code did not appear in output"
    end

    puts "Device code: #{device_info[:code]}"

    complete_device_flow(device_info[:url], device_info[:code])

    begin
      Timeout.timeout(timeout) do
        reader_thread.join
        wait_thread.join
      end
    rescue Timeout::Error
      Process.kill('KILL', wait_thread.pid) rescue nil
      reader_thread.kill
      raise "psql did not complete within #{timeout} seconds"
    end

    exit_status = wait_thread.value.exitstatus

    { output: output_buffer, exit_status: exit_status }
  end

  def verify_oauth_query_output(output)
    errors = []

    errors << "Expected 'current_user' column header" unless output.include?('current_user')
    errors << "Expected 'testuser' in query result" unless output.include?('testuser')
    errors << "Expected '(1 row)' in output" unless output.match?(/\(1 row\)/)
    errors << "Authentication failed" if output.match?(/authentication failed/i)
    errors << "Password authentication should not be attempted" if output.match?(/password.*failed/i)

    errors
  end
end
