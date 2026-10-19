"""Ordered public prices must not turn conditional promotions into fixed costs."""
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from activity_price_rules import (evaluate_condition, evaluate_price_rule,
    price_condition, price_rule_description, shared_price_context, shared_price_rule)


def context(checker='date$20991001-20991016', rate=0.9, ticket=False):
    return {'schema': 1, 'verified': True, 'unresolved': [],
            'globalDiscount': {'value': rate, 'checker': checker},
            'cases': {'futureVip': {'checker': 'vip'}, 'futureItem': {'checker': 'item$9001:2'}},
            'payment': {'kind': 'diamond', 'ticket': {'when': {'type': 'always', 'value': ticket},
                'minComponents': 2, 'maxDeduction': 200, 'selectionRequired': True,
                'deductAutomatically': False, 'canUseDiamondCoupon': True, 'canUseTicket': True}}}


def public(checker='', price='109', code='11-62'):
    return {'id': 999, 'discountChecker': checker, 'discountPrice': price, 'strengthenParams': code}


class PublicPriceRulesTest(unittest.TestCase):
    def test_same_catalog_re_evaluates_at_0200_boundaries_without_refresh(self):
        rule = shared_price_rule(public(), '999', context())
        for moment, expected in [('2099-10-01T01:59:59+08:00', 109),
                                  ('2099-10-01T02:00:00+08:00', 99),
                                  ('2099-10-16T01:59:59+08:00', 99),
                                  ('2099-10-16T02:00:00+08:00', 109)]:
            with self.subTest(moment=moment):
                quote = evaluate_price_rule(rule, moment)
                self.assertEqual((quote['state'], quote['amount']), ('known', expected))
        self.assertEqual(rule['preset'][0]['amount'], 109)

    def test_override_case_priority_is_ordered_and_missing_vip_is_unknown(self):
        rule = shared_price_rule(public(), '999$125/futureVip:79/futureItem:59', context('false'))
        date = '2099-10-18T10:00:00+08:00'
        self.assertEqual(evaluate_price_rule(rule, date)['state'], 'unknown')
        both = {'rmbVip': True, 'materials': {'4:9001': 2}}
        self.assertEqual(evaluate_price_rule(rule, date, both)['amount'], 79)
        second = {'rmbVip': False, 'materials': {'4:9001': 2}}
        self.assertEqual(evaluate_price_rule(rule, date, second)['amount'], 59)
        neither = {'rmbVip': False, 'materials': {'4:9001': 1}}
        self.assertEqual(evaluate_price_rule(rule, date, neither)['amount'], 125)

    def test_global_item_and_vip_discounts_need_both_facts(self):
        rule = shared_price_rule(public(), '999', context('vip&item$9001:2', 0.7))
        date = '2099-10-18T10:00:00+08:00'
        self.assertEqual(evaluate_price_rule(rule, date, {'rmbVip': True})['state'], 'unknown')
        self.assertEqual(evaluate_price_rule(rule, date, {'rmbVip': False})['amount'], 109)
        self.assertEqual(evaluate_price_rule(rule, date, {'rmbVip': True, 'materials': {'4:9001': 2}})['amount'], 77)

    def test_preset_first_matching_date_and_vip_or_offline(self):
        rule = shared_price_rule(public('date$20991001-20991016/vip', '49/89'), '999', context('false'))
        self.assertEqual(evaluate_price_rule(rule, '2099-10-02T10:00:00+08:00')['amount'], 49)
        self.assertEqual(evaluate_price_rule(rule, '2099-10-18T10:00:00+08:00')['state'], 'unknown')
        self.assertEqual(evaluate_price_rule(rule, '2099-10-18T10:00:00+08:00', {'rmbVip': False})['state'], 'offline')
        self.assertEqual(evaluate_price_rule(rule, '2099-10-18T10:00:00+08:00', {'rmbVip': True})['amount'], 89)

    def test_conditional_override_does_not_bypass_service_online_check(self):
        rule = shared_price_rule(public('false'), '999$futureVip:1', context('false'))
        quote = evaluate_price_rule(rule, '2099-10-18T10:00:00+08:00', {'rmbVip': True})
        self.assertEqual(quote['state'], 'offline')
        rule = shared_price_rule(public('false'), '999$120/futureVip:1', context('false'))
        self.assertEqual(evaluate_price_rule(rule, '2099-10-18T10:00:00+08:00', {'rmbVip': True})['amount'], 1)

    def test_zero_price_and_decimal_rounding_are_valid(self):
        rule = shared_price_rule(public(price='0'), '999', context())
        self.assertEqual(evaluate_price_rule(rule, '2099-10-02T10:00:00+08:00')['amount'], 0)
        rule = shared_price_rule(public(price='100'), '999', context(rate=0.29))
        self.assertEqual(evaluate_price_rule(rule, '2099-10-02T10:00:00+08:00')['amount'], 29)

    def test_ticket_maximum_is_never_an_automatic_reduction(self):
        rule = shared_price_rule(public(), '999', context('false', ticket=True))
        quote = evaluate_price_rule(rule, '2099-10-18T10:00:00+08:00')
        self.assertEqual(quote['amount'], 109)
        self.assertTrue(quote['ticketAvailable'])
        self.assertTrue(quote['quoteBeforeOptionalPaymentSelection'])
        single = shared_price_rule(public(code='34'), '999', context('false', ticket=True))
        self.assertFalse(evaluate_price_rule(single, '2099-10-18T10:00:00+08:00')['ticketAvailable'])

    def test_expired_date_short_circuits_unknown_account(self):
        condition = price_condition('vip&date$20991001-20991016')
        self.assertFalse(evaluate_condition(condition, '2099-10-18T10:00:00+08:00'))
        self.assertIsNone(evaluate_condition(condition, '2099-10-02T10:00:00+08:00'))

    def test_absent_material_is_unknown_unless_inventory_is_complete(self):
        condition = price_condition('item$9001:2')
        self.assertIsNone(evaluate_condition(condition, '2099-10-18T10:00:00+08:00', {'materials': {}}))
        self.assertFalse(evaluate_condition(condition, '2099-10-18T10:00:00+08:00', {'materials': {}, 'materialsComplete': True}))

    def test_official_description_fallback_not_confused_with_real_condition(self):
        self.assertTrue(evaluate_condition(price_condition('任意说明'), '2099-10-18T10:00:00+08:00'))
        self.assertFalse(evaluate_condition(price_condition('FaLsE'), '2099-10-18T10:00:00+08:00'))
        self.assertIsNone(evaluate_condition(price_condition('item$broken'), '2099-10-18T10:00:00+08:00'))

    def test_unverified_or_malformed_source_never_becomes_fixed_price(self):
        invalid = shared_price_rule(public(), '999', shared_price_context({}))
        self.assertTrue(invalid['unresolved'])
        self.assertEqual(evaluate_price_rule(invalid, '2099-10-18T10:00:00+08:00')['state'], 'unknown')
        invalid = shared_price_rule(public('vip', '1/2'), '999', context())
        self.assertTrue(invalid['unresolved'])
        invalid = shared_price_rule(public(), '999$unknown:1', context())
        self.assertTrue(invalid['unresolved'])
        self.assertIn('覆盖价格缺少对应条件', price_rule_description(invalid))

    def test_input_is_not_mutated_and_no_timestamp_quote_is_persisted(self):
        source, ctx = public(), context()
        before = copy.deepcopy((source, ctx))
        rule = shared_price_rule(source, '999', ctx)
        evaluate_price_rule(rule, '2099-10-02T10:00:00+08:00')
        self.assertEqual((source, ctx), before)
        self.assertNotIn('cost', rule)
        self.assertNotIn('referenceDate', rule)


if __name__ == '__main__':
    unittest.main()
