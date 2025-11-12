#!/usr/bin/env ruby

require 'selenium-webdriver'
require 'optparse'

# Parse command line arguments
options = {}
OptionParser.new do |opts|
  opts.banner = "Usage: keycloak_device_flow.rb [options]"

  opts.on("-u", "--user USERNAME", "Keycloak username") do |u|
    options[:username] = u
  end

  opts.on("-p", "--password PASSWORD", "Keycloak password") do |p|
    options[:password] = p
  end

  opts.on("-c", "--code DEVICE_CODE", "OAuth device code") do |c|
    options[:device_code] = c
  end

  opts.on("-l", "--url URL", "Keycloak device flow URL") do |l|
    options[:url] = l
  end

  opts.on("-h", "--headless", "Run in headless mode") do
    options[:headless] = true
  end

  opts.on("--help", "Show this help message") do
    puts opts
    exit
  end
end.parse!

# Validate required parameters
required = [:username, :password, :device_code, :url]
missing = required.select { |param| options[param].nil? }

unless missing.empty?
  puts "Error: Missing required parameters: #{missing.join(', ')}"
  puts "Use --help for usage information"
  exit 1
end

begin
  chrome_options = Selenium::WebDriver::Chrome::Options.new
  chrome_options.add_argument('--headless=new') if options[:headless]
  chrome_options.add_argument('--no-sandbox')
  chrome_options.add_argument('--disable-dev-shm-usage')
  chrome_options.add_argument('--disable-gpu')
  chrome_options.add_argument('--disable-software-rasterizer')
  chrome_options.add_argument('--ignore-certificate-errors')
  chrome_options.add_argument('--ignore-ssl-errors')
  chrome_options.binary = '/usr/bin/chromium-browser' if File.exist?('/usr/bin/chromium-browser')

  driver = Selenium::WebDriver.for :chrome, options: chrome_options
  wait = Selenium::WebDriver::Wait.new(timeout: 30)

  # Navigate to the device flow URL
  driver.navigate.to options[:url]

  # Enter device code
  device_code_input = wait.until do
    element = driver.find_element(id: 'device-user-code')
    element if element.displayed?
  end
  device_code_input.send_keys(options[:device_code])

  # Submit device code
  submit_button = driver.find_element(css: 'input[type="submit"]')
  submit_button.click

  # Wait for login page and enter username
  username_input = wait.until do
    element = driver.find_element(id: 'username')
    element if element.displayed?
  end
  username_input.send_keys(options[:username])

  # Enter password
  password_input = driver.find_element(id: 'password')
  password_input.send_keys(options[:password])

  # Submit login form
  login_button = driver.find_element(id: 'kc-login')
  login_button.click

  # Wait for consent page if it exists
  sleep 2
  begin
    consent_button = wait.until do
      element = driver.find_element(id: 'kc-login')
      element if element.displayed?
    rescue Selenium::WebDriver::Error::NoSuchElementError
      nil
    end

    consent_button.click if consent_button
  rescue Selenium::WebDriver::Error::TimeoutError
    # No consent page
  end

  sleep 2
  puts "✓ Device flow authentication completed"
  exit 0

rescue StandardError => e
  puts "Error: #{e.message}"
  exit 1

ensure
  driver.quit if driver
end
