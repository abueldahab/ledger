#include "t_expr.h"

#include "expr.h"

using namespace ledger;

CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(ValueExprTestCase, "expr");

void ValueExprTestCase::setUp()
{
  amount_t::initialize();
  expr_t::initialize();
}

void ValueExprTestCase::tearDown()
{
  expr_t::shutdown();
  amount_t::shutdown();
}

void ValueExprTestCase::testConstructors()
{
}
